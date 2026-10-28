#include "node.h"
#include <chrono>
#include <fstream>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "civetweb.h"
#include "compat/net_compat.h"
#include "config.h"
#include "connection.h"
#include "dnssd.h"
#include "is12.h"
#include "log.h"
#include "ptp/ptpclient.h"
#include "rtp/rtpreceiver.h"
#include "rtp/rtpsender.h"

using namespace pan::nmos;
using json = nlohmann::json;

namespace
{
    struct UrlParts { std::string sHost; int nPort = 80; std::string sPath; };

    std::optional<UrlParts> ParseUrl(const std::string& sUrl)
    {
        auto nScheme = sUrl.find("://");
        if(nScheme == std::string::npos) { return std::nullopt; }
        auto sRest = sUrl.substr(nScheme+3);
        UrlParts parts;
        auto nSlash = sRest.find('/');
        auto sHostPort = nSlash == std::string::npos ? sRest : sRest.substr(0, nSlash);
        parts.sPath = nSlash == std::string::npos ? "/" : sRest.substr(nSlash);
        if(auto nColon = sHostPort.find(':'); nColon != std::string::npos)
        {
            parts.sHost = sHostPort.substr(0, nColon);
            parts.nPort = atoi(sHostPort.c_str()+nColon+1);
        }
        else
        {
            parts.sHost = sHostPort;
        }
        return parts;
    }
}

std::optional<std::string> pan::nmos::HttpText(const std::string& sMethod, const std::string& sUrl,
                                               const std::string* pBody, int& nStatus)
{
    //raw socket client with a HARD connect timeout. the civetweb client blocks
    //in connect() for minutes on unreachable registries, which froze every
    //caller (topology changes, shutdown joins, the whole web api)
    nStatus = 0;
    auto parts = ParseUrl(sUrl);
    if(!parts) { return std::nullopt; }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* pAddr = nullptr;
    if(getaddrinfo(parts->sHost.c_str(), std::to_string(parts->nPort).c_str(), &hints, &pAddr) != 0 || !pAddr)
    {
        return std::nullopt;
    }

    int nSocket = socket(pAddr->ai_family, pAddr->ai_socktype, 0);
    if(nSocket < 0) { freeaddrinfo(pAddr); return std::nullopt; }

    //non-blocking connect with 2s deadline
    fcntl(nSocket, F_SETFL, fcntl(nSocket, F_GETFL, 0) | O_NONBLOCK);
    int nResult = connect(nSocket, pAddr->ai_addr, pAddr->ai_addrlen);
    freeaddrinfo(pAddr);
    if(nResult < 0 && errno == EINPROGRESS)
    {
        pollfd pfd{nSocket, POLLOUT, 0};
        if(poll(&pfd, 1, 1000) <= 0) { close(nSocket); return std::nullopt; }
        int nError = 0;
        socklen_t nLen = sizeof(nError);
        getsockopt(nSocket, SOL_SOCKET, SO_ERROR, &nError, &nLen);
        if(nError != 0) { close(nSocket); return std::nullopt; }
    }
    else if(nResult < 0) { close(nSocket); return std::nullopt; }
    fcntl(nSocket, F_SETFL, fcntl(nSocket, F_GETFL, 0) & ~O_NONBLOCK);
    timeval tv{3, 0};
    setsockopt(nSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(nSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string sBody = pBody ? *pBody : "";
    std::string sRequest = sMethod + " " + parts->sPath + " HTTP/1.1\r\nHost: " + parts->sHost +
        "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(sBody.size()) + "\r\n\r\n" + sBody;
    if(send(nSocket, sRequest.data(), sRequest.size(), 0) != ssize_t(sRequest.size()))
    {
        close(nSocket);
        return std::nullopt;
    }

    std::string sResponse;
    char buffer[4096];
    ssize_t nRead;
    while((nRead = recv(nSocket, buffer, sizeof(buffer), 0)) > 0)
    {
        sResponse.append(buffer, size_t(nRead));
        if(sResponse.size() > 16*1024*1024) { break; }
    }
    close(nSocket);

    //"HTTP/1.1 200 OK" + headers + body
    if(sResponse.rfind("HTTP/", 0) != 0) { return std::nullopt; }
    if(auto nSpace = sResponse.find(' '); nSpace != std::string::npos)
    {
        nStatus = atoi(sResponse.c_str() + nSpace + 1);
    }
    auto nHeaderEnd = sResponse.find("\r\n\r\n");
    if(nHeaderEnd == std::string::npos) { return std::string(); }
    return sResponse.substr(nHeaderEnd + 4);
}

std::optional<json> pan::nmos::HttpJson(const std::string& sMethod, const std::string& sUrl,
                                        const json* pBody, int& nStatus)
{
    std::string sBody = pBody ? pBody->dump() : "";
    auto sResponse = HttpText(sMethod, sUrl, pBody ? &sBody : nullptr, nStatus);
    if(!sResponse) { return std::nullopt; }
    if(sResponse->empty()) { return json(); }
    try { return json::parse(*sResponse); }
    catch(...) { return json(); }
}

namespace
{
    std::string InterfaceMac(const std::string& sInterface)
    {
        auto mac = pan::compat::InterfaceMac(sInterface);
        char buffer[18];
        snprintf(buffer, sizeof(buffer), "%02x-%02x-%02x-%02x-%02x-%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return buffer;
    }
}

std::string pan::nmos::MakeUuid(const std::string& sSeed)
{
    char sHash[33] = {0};
    mg_md5(sHash, sSeed.c_str(), nullptr);
    std::string s(sHash);
    //rfc 4122: version nibble = 3 (name based, md5), variant nibble in [89ab]
    s[12] = '3';
    const char* HEX = "0123456789abcdef";
    auto nVariant = static_cast<size_t>(strchr(HEX, s[16]) - HEX);
    s[16] = HEX[(nVariant & 0x3) | 0x8];
    return s.substr(0,8)+"-"+s.substr(8,4)+"-"+s.substr(12,4)+"-"+s.substr(16,4)+"-"+s.substr(20,12);
}

NmosNode::NmosNode(ptp::PtpClient& ptpClient,
                   std::vector<std::unique_ptr<rtp::RtpSender>>& vSenders,
                   std::vector<std::unique_ptr<rtp::RtpReceiver>>& vReceivers) :
    m_ptp(ptpClient),
    m_vSenders(vSenders),
    m_vReceivers(vReceivers)
{
}

size_t NmosNode::SenderIndex(const std::string& sId) const
{
    for(size_t n = 0; n < m_vSenderIds.size(); n++) { if(m_vSenderIds[n] == sId) { return n; } }
    return std::string::npos;
}

size_t NmosNode::ReceiverIndex(const std::string& sId) const
{
    for(size_t n = 0; n < m_vReceiverIds.size(); n++) { if(m_vReceiverIds[n] == sId) { return n; } }
    return std::string::npos;
}

nlohmann::json NmosNode::SenderConfig(size_t n)
{
    auto js = Config::Get().GetJson();
    if(js.contains("senders") && js["senders"].is_array() && n < js["senders"].size())
    {
        return js["senders"][n];
    }
    return json::object();
}

NmosNode::~NmosNode()
{
    Stop();
}

std::string NmosNode::Version() const
{
    //the timestamp is captured when the resource last CHANGED (BumpVersion),
    //not at read time - is-04 requires the version to be stable between
    //changes, and the is-13 tests verify patch response == node api version
    return std::to_string(m_nVersionTime.load()) + ":" + std::to_string(m_nVersion.load() % 1000000000);
}

void NmosNode::BumpVersion()
{
    m_nVersionTime = uint64_t(time(nullptr));
    m_nVersion++;
}

std::vector<std::string> NmosNode::GetHostIps() const
{
    //configured interfaces first; if none of them carries an ipv4 (stale
    //config after a site change), fall back to any real interface - a node
    //registered with empty hrefs is undiscoverable, which is worse than a
    //best-guess address
    std::vector<std::string> vIps, vFallback;
    ifaddrs* pList = nullptr;
    if(getifaddrs(&pList) != 0) { return vIps; }
    for(auto* p = pList; p; p = p->ifa_next)
    {
        if(!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) { continue; }
        char buffer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buffer, sizeof(buffer));
        std::string sName = p->ifa_name ? p->ifa_name : "";
        for(const auto& sInterface : m_vInterfaces)
        {
            if(sInterface == sName) { vIps.emplace_back(buffer); }
        }
        //fallback candidates: skip loopback and virtual/vm interfaces
        bool bVirtual = false;
        for(const char* sSkip : {"lo", "utun", "awdl", "llw", "gif", "stf", "anpi", "bridge", "ap", "vmnet"})
        {
            if(sName.rfind(sSkip, 0) == 0) { bVirtual = true; break; }
        }
        if(!bVirtual) { vFallback.emplace_back(buffer); }
    }
    freeifaddrs(pList);
    if(vIps.empty())
    {
        if(!vFallback.empty())
        {
            LOG_WARN("nmos") << "configured interfaces carry no ipv4 - announcing " << vFallback[0]
                             << " instead (check network.interface_primary)";
        }
        return vFallback;
    }
    return vIps;
}

namespace
{
    //BCP-002-02 asset distinguishing information: read-only inventory tags on
    //self and device. merged AFTER the user tags so they cannot be shadowed,
    //and the annotation api refuses to modify them (500, per IS-13).
    json AssetTags()
    {
        auto sInstance = pan::Config::Get().GetValue<std::string>("device.uuid_seed", "");
        auto sFunction = pan::Config::Get().GetValue<std::string>("device.asset_function", "AES67 Virtual Soundcard");
        return {{"urn:x-nmos:tag:asset:manufacturer", {"Gemini2350"}},
                {"urn:x-nmos:tag:asset:product", {"AES67eleven"}},
                {"urn:x-nmos:tag:asset:instance-id", {sInstance}},
                {"urn:x-nmos:tag:asset:function", {sFunction}}};
    }
    bool IsReadOnlyTag(const std::string& sName)
    {
        return sName.rfind("urn:x-nmos:tag:grouphint/", 0) == 0 ||
               sName.rfind("urn:x-nmos:tag:asset:", 0) == 0;
    }
    void MergeAssetTags(json& jsTags)
    {
        for(auto& [sKey, jsValue] : AssetTags().items()) { jsTags[sKey] = jsValue; }
    }
}

json NmosNode::BuildSelf() const
{
    auto sLabel = Config::Get().GetValue<std::string>("device.label", "");
    if(sLabel.empty()) { sLabel = "AES67eleven"; }
    json js;
    js["id"] = m_sNodeId;
    js["version"] = Version();
    js["label"] = sLabel;
    js["description"] = Config::Get().GetValue<std::string>("device.description", "");
    js["tags"] = Config::Get().GetValue<json>("device.tags", json::object());
    MergeAssetTags(js["tags"]);
    js["hostname"] = m_sHostname;
    js["caps"] = json::object();
    js["api"]["versions"] = {"v1.3"};
    js["api"]["endpoints"] = json::array();
    js["services"] = json::array();
    js["clocks"] = json::array();
    js["interfaces"] = json::array();

    auto sGm = m_ptp.GrandmasterId();
    if(!sGm.empty())
    {
        for(auto& c : sGm) { if(c == ':') { c = '-'; } }    //IS-04 wants dashes
        js["clocks"].push_back({{"name", "clk0"}, {"ref_type", "ptp"}, {"traceable", false},
                                {"version", "IEEE1588-2008"}, {"gmid", sGm}, {"locked", m_ptp.IsSynced()}});
    }
    else
    {
        js["clocks"].push_back({{"name", "clk0"}, {"ref_type", "internal"}});
    }

    for(const auto& sIp : GetHostIps())
    {
        js["api"]["endpoints"].push_back({{"host", sIp}, {"port", m_nPort}, {"protocol", "http"}});
        //IS-13 discovery: the annotation api is a service of the node, one
        //entry per interface (IS-13 v1.0, Interoperability - IS-04)
        js["services"].push_back({{"type", "urn:x-nmos:service:annotation/v1.0"},
                                  {"href", "http://"+sIp+":"+std::to_string(m_nPort)+"/x-nmos/annotation/v1.0/"},
                                  {"authorization", false}});    //field exists since IS-04 v1.3
    }
    for(const auto& sInterface : m_vInterfaces)
    {
        js["interfaces"].push_back({{"name", sInterface}, {"chassis_id", nullptr}, {"port_id", InterfaceMac(sInterface)}});
    }
    js["href"] = js["api"]["endpoints"].empty() ? "" :
        "http://" + js["api"]["endpoints"][0]["host"].get<std::string>() + ":" + std::to_string(m_nPort);
    return js;
}

json NmosNode::BuildDevice() const
{
    json js;
    js["id"] = m_sDeviceId;
    js["version"] = Version();
    {
        auto sDeviceLabel = Config::Get().GetValue<std::string>("device.label", "");
        js["label"] = sDeviceLabel.empty() ? "AES67eleven" : sDeviceLabel;
    }
    {
        auto sDescription = Config::Get().GetValue<std::string>("device.description", "");
        js["description"] = sDescription.empty() ? "AES67 sender/receiver" : sDescription;
    }
    js["tags"] = Config::Get().GetValue<json>("device.tags", json::object());
    MergeAssetTags(js["tags"]);
    js["type"] = "urn:x-nmos:device:generic";
    js["node_id"] = m_sNodeId;
    js["senders"] = m_vSenderIds;
    js["receivers"] = m_vReceiverIds;
    js["controls"] = json::array();
    for(const auto& sIp : GetHostIps())
    {
        js["controls"].push_back({{"type", "urn:x-nmos:control:sr-ctrl/v1.1"},
                                  {"href", "http://"+sIp+":"+std::to_string(m_nPort)+"/x-nmos/connection/v1.1"}});
        js["controls"].push_back({{"type", "urn:x-nmos:control:ncp/v1.0"},
                                  {"href", "ws://"+sIp+":"+std::to_string(m_nPort)+"/x-nmos/ncp/v1.0"}});
    }
    return js;
}

json NmosNode::BuildSource(size_t n) const
{
    auto jsSender = SenderConfig(n);
    auto sLabel = jsSender.value("label", "");
    if(sLabel.empty())
    {
        sLabel = Config::Get().GetValue<std::string>("device.label", "AES67eleven") + " " + std::to_string(n+1);
    }
    int nChannels = jsSender.value("channels", 8);
    int nOffset = jsSender.value("channel_offset", int(n)*8);
    //channel_map (matrix routing) wins over the contiguous offset
    std::vector<int> vMap;
    if(jsSender.contains("channel_map") && jsSender["channel_map"].is_array())
    {
        for(const auto& jsCh : jsSender["channel_map"]) { vMap.push_back(jsCh.get<int>()); }
    }
    else
    {
        for(int nCh = 0; nCh < nChannels; nCh++) { vMap.push_back(nOffset + nCh); }
    }

    json js;
    js["id"] = m_vSourceIds[n];
    js["version"] = Version();
    auto sSourceLabel = jsSender.value("source_label", "");
    js["label"] = sSourceLabel.empty() ? sLabel + " source" : sSourceLabel;
    js["description"] = jsSender.value("source_description", "");
    js["tags"] = jsSender.value("source_tags", json::object());
    js["device_id"] = m_sDeviceId;
    js["parents"] = json::array();
    js["clock_name"] = "clk0";
    js["caps"] = json::object();
    js["format"] = "urn:x-nmos:format:audio";
    js["channels"] = json::array();
    for(size_t nCh = 0; nCh < vMap.size(); nCh++)
    {
        //is-04 schema: undefined channel symbols are U01..U64 (two digits)
        char sSymbol[8];
        snprintf(sSymbol, sizeof(sSymbol), "U%02zu", nCh+1);
        js["channels"].push_back({{"label", "Ch " + std::to_string(vMap[nCh]+1)},
                                  {"symbol", sSymbol}});
    }
    return js;
}

json NmosNode::BuildFlow(size_t n) const
{
    auto jsSender = SenderConfig(n);
    json js;
    js["id"] = m_vFlowIds[n];
    js["version"] = Version();
    auto sFlowLabel = jsSender.value("flow_label", "");
    js["label"] = sFlowLabel.empty() ?
        Config::Get().GetValue<std::string>("device.label", "AES67eleven") + " flow " + std::to_string(n+1) : sFlowLabel;
    js["description"] = jsSender.value("flow_description", "");
    js["tags"] = jsSender.value("flow_tags", json::object());
    js["device_id"] = m_sDeviceId;
    js["source_id"] = m_vSourceIds[n];
    js["parents"] = json::array();
    js["format"] = "urn:x-nmos:format:audio";
    js["media_type"] = jsSender.value("bits", 24) == 16 ? "audio/L16" : "audio/L24";
    js["sample_rate"] = {{"numerator", 48000}};
    js["bit_depth"] = jsSender.value("bits", 24);
    return js;
}

json NmosNode::BuildSender(size_t n) const
{
    auto jsSender = SenderConfig(n);
    auto sLabel = jsSender.value("label", "");
    if(sLabel.empty())
    {
        sLabel = Config::Get().GetValue<std::string>("device.label", "AES67eleven") + " sender " + std::to_string(n+1);
    }

    json js;
    js["id"] = m_vSenderIds[n];
    js["version"] = Version();
    js["label"] = sLabel;
    js["description"] = jsSender.value("description", "");
    js["tags"] = jsSender.value("tags", json::object());
    js["flow_id"] = m_vFlowIds[n];
    js["transport"] = "urn:x-nmos:transport:rtp.mcast";
    js["device_id"] = m_sDeviceId;
    js["interface_bindings"] = m_vInterfaces;
    auto vIps = GetHostIps();
    js["manifest_href"] = vIps.empty() ? "" :
        "http://"+vIps[0]+":"+std::to_string(m_nPort)+"/x-nmos/connection/v1.1/single/senders/"+m_vSenderIds[n]+"/transportfile";
    js["subscription"] = {{"receiver_id", nullptr}, {"active", m_vSenders[n]->IsRunning()}};
    return js;
}

json NmosNode::BuildReceiver(size_t n) const
{
    json js;
    json jsReceiver;
    {
        auto jsConfig = Config::Get().GetJson();
        if(jsConfig.contains("receivers") && jsConfig["receivers"].is_array() && n < jsConfig["receivers"].size())
        {
            jsReceiver = jsConfig["receivers"][n];
        }
    }
    auto sLabel = jsReceiver.value("label", "");
    if(sLabel.empty())
    {
        sLabel = Config::Get().GetValue<std::string>("device.label", "AES67eleven") + " receiver " + std::to_string(n+1);
    }
    js["id"] = m_vReceiverIds[n];
    js["version"] = Version();
    js["label"] = sLabel;
    js["description"] = jsReceiver.value("description", "");
    js["tags"] = jsReceiver.value("tags", json::object());
    js["device_id"] = m_sDeviceId;
    js["transport"] = "urn:x-nmos:transport:rtp.mcast";
    js["interface_bindings"] = m_vInterfaces;
    js["format"] = "urn:x-nmos:format:audio";
    js["caps"]["media_types"] = {"audio/L24", "audio/L16"};
    //bcp-004-01: tell strict controllers what this receiver really accepts
    json jsConstraints;
    jsConstraints["urn:x-nmos:cap:format:media_type"] = {{"enum", {"audio/L24", "audio/L16"}}};
    jsConstraints["urn:x-nmos:cap:format:sample_rate"] = {{"enum", json::array({{{"numerator", 48000}}})}};
    jsConstraints["urn:x-nmos:cap:format:channel_count"] = {{"minimum", 1}, {"maximum", 8}};
    jsConstraints["urn:x-nmos:cap:transport:packet_time"] = {{"minimum", 0.125}, {"maximum", 4.0}};
    js["caps"]["constraint_sets"] = json::array({jsConstraints});
    js["caps"]["version"] = Version();
    js["subscription"] = {{"sender_id", m_pConnection ? m_pConnection->ReceiverSenderId(n) : json()},
                          {"active", m_vReceivers[n]->IsRunning()}};
    return js;
}

bool NmosNode::Start(int nPort, const std::vector<std::string>& vInterfaces, Callbacks callbacks)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    StopLocked();
    m_nPort = nPort;
    m_vInterfaces = vInterfaces;

    char sHostname[128] = {0};
    gethostname(sHostname, sizeof(sHostname)-1);
    m_sHostname = sHostname;

    //ids derive from the frozen uuid seed, not the live hostname - dhcp
    //renames at other sites must never change our identity
    auto sIdSeed = Config::Get().GetValue<std::string>("device.uuid_seed", m_sHostname);
    m_sNodeId = MakeUuid(sIdSeed + ":pan:node");
    m_sDeviceId = MakeUuid(sIdSeed + ":pan:device");
    //ids derive from each stream's persistent config uid, so they survive
    //add/remove of other streams. uid "legacy" keeps the pre-v2 single-stream
    //ids alive - existing controller routes stay valid.
    auto StreamUid = [](const char* sArray, size_t n) -> std::string
    {
        auto js = Config::Get().GetJson();
        if(js.contains(sArray) && js[sArray].is_array() && n < js[sArray].size())
        {
            return js[sArray][n].value("uid", "");
        }
        return std::to_string(n);
    };
    auto Seed = [&](const std::string& sKind, const std::string& sUid) -> std::string
    {
        return sUid == "legacy" ? sIdSeed + ":pan:" + sKind
                                : sIdSeed + ":pan:" + sKind + ":" + sUid;
    };
    m_vSourceIds.clear(); m_vFlowIds.clear(); m_vSenderIds.clear(); m_vReceiverIds.clear();
    for(size_t n = 0; n < m_vSenders.size(); n++)
    {
        auto sUid = StreamUid("senders", n);
        m_vSourceIds.push_back(MakeUuid(Seed("source", sUid)));
        m_vFlowIds.push_back(MakeUuid(Seed("flow", sUid)));
        m_vSenderIds.push_back(MakeUuid(Seed("sender", sUid)));
    }
    for(size_t n = 0; n < m_vReceivers.size(); n++)
    {
        m_vReceiverIds.push_back(MakeUuid(Seed("receiver", StreamUid("receivers", n))));
    }

    std::string sPort = std::to_string(nPort);
    const char* options[] = {
        "listening_ports", sPort.c_str(),
        //answer cors preflights with the full method list - civetweb handles
        //OPTIONS itself and would otherwise just echo the requested method
        "access_control_allow_methods", "GET, PUT, POST, PATCH, DELETE, HEAD, OPTIONS",
        "num_threads", "6",
        "enable_directory_listing", "no",
        nullptr
    };
    m_pServer = mg_start(nullptr, nullptr, options);
    if(!m_pServer)
    {
        LOG_ERROR("nmos") << "cannot start http server on port " << nPort;
        return false;
    }

    m_pConnection = std::make_unique<ConnectionApi>(*this, std::move(callbacks.onReceiver), std::move(callbacks.onSender));
    m_pConnection->Register(m_pServer);
    m_pIs12 = std::make_unique<Is12Server>(*this);
    m_pIs12->Register(m_pServer);
    RegisterNodeApi();
    RegisterAnnotationApi();

    m_bRun = true;
    m_regThread = std::thread(&NmosNode::RegistrationLoop, this);
    LOG_INFO("nmos") << "node " << m_sNodeId << " on port " << nPort;
    return true;
}

void NmosNode::Stop()
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    StopLocked();
}

void NmosNode::StopLocked()
{
    m_bRun = false;
    if(m_regThread.joinable()) { m_regThread.join(); }
    if(m_pServer)
    {
        mg_stop(m_pServer);
        m_pServer = nullptr;
    }
    m_pConnection.reset();
    m_pIs12.reset();
}

bool NmosNode::RegisterAll(const std::string& sRegistryUrl)
{
    auto Post = [&](const std::string& sType, const json& jsData) -> bool
    {
        json jsBody = {{"type", sType}, {"data", jsData}};
        int nStatus = 0;
        auto jsResponse = HttpJson("POST", sRegistryUrl + "/x-nmos/registration/v1.3/resource", &jsBody, nStatus);
        if(nStatus == 200 || nStatus == 201)
        {
            //a registration api always answers with the resource as json. a
            //web server (spa fallback) answers any path with 200 + html - do
            //not let that masquerade as a successful registration
            if(jsResponse) { return true; }
            LOG_WARN("nmos") << "register " << sType << " -> " << nStatus
                             << " without json - that url is not a registration api (web ui?)";
            return false;
        }
        LOG_WARN("nmos") << "register " << sType << " -> " << nStatus
                         << (jsResponse ? " "+jsResponse->dump().substr(0, 300) : "");
        return false;
    };

    if(!m_bRun) { return false; }
    if(!Post("node", BuildSelf()) || !Post("device", BuildDevice())) { return false; }
    for(size_t n = 0; n < m_vSenderIds.size(); n++)
    {
        if(!m_bRun) { return false; }
        if(!Post("source", BuildSource(n)) || !Post("flow", BuildFlow(n)) || !Post("sender", BuildSender(n)))
        {
            return false;
        }
    }
    for(size_t n = 0; n < m_vReceiverIds.size(); n++)
    {
        if(!m_bRun) { return false; }
        if(!Post("receiver", BuildReceiver(n))) { return false; }
    }
    return true;
}

void NmosNode::RegistrationLoop()
{
    int nSinceDiscovery = 999;
    std::vector<RegistryEntry> vRegistries;
    bool bRegistered = false;

    while(m_bRun)
    {
        auto sOverride = Config::Get().GetValue<std::string>("nmos.registry_override", "");

        if(!bRegistered)
        {
            if(!sOverride.empty())
            {
                //accept "host:port" without scheme - the field is hand-typed
                if(sOverride.rfind("http://", 0) != 0 && sOverride.rfind("https://", 0) != 0)
                {
                    sOverride = "http://" + sOverride;
                }
                vRegistries = {{sOverride, 0, ""}};
            }
            else if(++nSinceDiscovery >= 5)
            {
                vRegistries = DiscoverRegistries();
                nSinceDiscovery = 0;
            }

            for(const auto& registry : vRegistries)
            {
                if(RegisterAll(registry.sUrl))
                {
                    std::lock_guard<std::mutex> lg(m_mutex);
                    m_sRegistry = registry.sUrl;
                    m_sRegistryStatus = "registered";
                    bRegistered = true;
                    m_nRegisteredVersion = m_nVersion.load();
                    LOG_INFO("nmos") << "registered with " << registry.sUrl;
                    break;
                }
            }
            if(!bRegistered)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                m_sRegistry.clear();
                m_sRegistryStatus = vRegistries.empty() ? "no registry found" : "registration failed";
            }
        }
        else
        {
            //heartbeat + push changed resources
            std::string sRegistry;
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                sRegistry = m_sRegistry;
            }
            int nStatus = 0;
            json jsEmpty = json::object();
            auto jsHealth = HttpJson("POST", sRegistry + "/x-nmos/registration/v1.3/health/nodes/" + m_sNodeId,
                                     &jsEmpty, nStatus);
            if(nStatus >= 200 && nStatus < 300 && !jsHealth)
            {
                nStatus = 0;    //200 + html is a web server, not a registry
            }
            if(nStatus == 404)
            {
                bRegistered = RegisterAll(sRegistry);   //registry lost us - re-register
            }
            else if(nStatus < 200 || nStatus >= 300)
            {
                LOG_WARN("nmos") << "heartbeat -> " << nStatus << " - rediscovering";
                bRegistered = false;
                nSinceDiscovery = 999;
            }
            else if(m_nVersion.load() != m_nRegisteredVersion.load())
            {
                if(RegisterAll(sRegistry)) { m_nRegisteredVersion = m_nVersion.load(); }
            }
        }

        for(int i = 0; i < 25 && m_bRun; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(bRegistered ? 200 : 80));
            //push is-05/config changes to the registry right away instead of
            //waiting out the full heartbeat interval - controllers see the
            //switch immediately
            if(bRegistered && m_nVersion.load() != m_nRegisteredVersion.load()) { break; }
        }
    }

    //unregister on shutdown
    std::string sRegistry;
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        sRegistry = m_sRegistry;
    }
    if(!sRegistry.empty())
    {
        int nStatus = 0;
        HttpJson("DELETE", sRegistry + "/x-nmos/registration/v1.3/resource/nodes/" + m_sNodeId, nullptr, nStatus);
    }
}

namespace
{
    void SendJson(mg_connection* pConn, int nStatus, const json& js)
    {
        auto sBody = js.dump();
        mg_printf(pConn, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  nStatus, nStatus < 300 ? "OK" : "Error", sBody.size());
        mg_write(pConn, sBody.data(), sBody.size());
    }
}

void NmosNode::RegisterNodeApi()
{
    //IS-04 node api (read only) - one handler for the whole subtree
    mg_set_request_handler(m_pServer, "/x-nmos/node", [](mg_connection* pConn, void* pUser) -> int
    {
        auto* pNode = static_cast<NmosNode*>(pUser);
        std::string sUri(mg_get_request_info(pConn)->local_uri);
        if(!sUri.empty() && sUri.back() == '/') { sUri.pop_back(); }

        auto Senders = [&]() { json js = json::array(); for(size_t n = 0; n < pNode->SenderCount(); n++) { js.push_back(pNode->BuildSender(n)); } return js; };
        auto Receivers = [&]() { json js = json::array(); for(size_t n = 0; n < pNode->ReceiverCount(); n++) { js.push_back(pNode->BuildReceiver(n)); } return js; };
        auto Sources = [&]() { json js = json::array(); for(size_t n = 0; n < pNode->SenderCount(); n++) { js.push_back(pNode->BuildSource(n)); } return js; };
        auto Flows = [&]() { json js = json::array(); for(size_t n = 0; n < pNode->SenderCount(); n++) { js.push_back(pNode->BuildFlow(n)); } return js; };

        if(sUri == "/x-nmos/node")                    { SendJson(pConn, 200, json::array({"v1.3/"})); return 200; }
        if(sUri == "/x-nmos/node/v1.3")               { SendJson(pConn, 200, json::array({"self/", "devices/", "sources/", "flows/", "senders/", "receivers/"})); return 200; }
        if(sUri == "/x-nmos/node/v1.3/self")          { SendJson(pConn, 200, pNode->BuildSelf()); return 200; }
        if(sUri == "/x-nmos/node/v1.3/devices")       { SendJson(pConn, 200, json::array({pNode->BuildDevice()})); return 200; }
        if(sUri == "/x-nmos/node/v1.3/devices/"+pNode->DeviceId()) { SendJson(pConn, 200, pNode->BuildDevice()); return 200; }
        if(sUri == "/x-nmos/node/v1.3/sources")       { SendJson(pConn, 200, Sources()); return 200; }
        if(sUri == "/x-nmos/node/v1.3/flows")         { SendJson(pConn, 200, Flows()); return 200; }
        if(sUri == "/x-nmos/node/v1.3/senders")       { SendJson(pConn, 200, Senders()); return 200; }
        if(sUri == "/x-nmos/node/v1.3/receivers")     { SendJson(pConn, 200, Receivers()); return 200; }
        for(size_t n = 0; n < pNode->SenderCount(); n++)
        {
            if(sUri == "/x-nmos/node/v1.3/senders/"+pNode->SenderId(n)) { SendJson(pConn, 200, pNode->BuildSender(n)); return 200; }
            if(sUri == "/x-nmos/node/v1.3/sources/"+pNode->SourceId(n)) { SendJson(pConn, 200, pNode->BuildSource(n)); return 200; }
            if(sUri == "/x-nmos/node/v1.3/flows/"+pNode->FlowId(n))     { SendJson(pConn, 200, pNode->BuildFlow(n)); return 200; }
        }
        for(size_t n = 0; n < pNode->ReceiverCount(); n++)
        {
            if(sUri == "/x-nmos/node/v1.3/receivers/"+pNode->ReceiverId(n)) { SendJson(pConn, 200, pNode->BuildReceiver(n)); return 200; }
        }
        SendJson(pConn, 404, {{"code", 404}, {"error", "not found"}, {"debug", nullptr}});
        return 200;
    }, this);

    mg_set_request_handler(m_pServer, "/x-nmos$", [](mg_connection* pConn, void*) -> int
    {
        SendJson(pConn, 200, json::array({"annotation/", "node/", "connection/"}));
        return 200;
    }, this);
}


namespace
{
    void SendJsonCors(mg_connection* pConn, int nStatus, const json& js, const char* sMethods)
    {
        auto sBody = js.dump();
        mg_printf(pConn, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: %s\r\n"
                         "Access-Control-Allow-Headers: Content-Type\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                  nStatus, nStatus < 300 ? "OK" : "Error", sMethods, sBody.size());
        mg_write(pConn, sBody.data(), sBody.size());
    }
    constexpr const char* METHODS_RO = "GET, HEAD, OPTIONS";
    constexpr const char* METHODS_RW = "GET, PATCH, HEAD, OPTIONS";
}

void NmosNode::RegisterAnnotationApi()
{
    //IS-13 annotation api: controllers may rename label/description/tags of
    //self, device, senders and receivers. changes persist in the config and
    //bump the IS-04 version, so the registry picks them up right away.
    mg_set_request_handler(m_pServer, "/x-nmos/annotation", [](mg_connection* pConn, void* pUser) -> int
    {
        auto* pNode = static_cast<NmosNode*>(pUser);
        const auto* pInfo = mg_get_request_info(pConn);
        std::string sUri(pInfo->local_uri);
        std::string sMethod(pInfo->request_method);
        if(!sUri.empty() && sUri.back() == '/') { sUri.pop_back(); }

        if(sMethod == "OPTIONS")
        {
            mg_printf(pConn, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Access-Control-Allow-Methods: GET, PATCH, OPTIONS\r\n"
                             "Access-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n");
            return 204;
        }

        //strip the annotation view out of the full is-04 resource - that way
        //labels, defaults and versions can never drift apart
        auto View = [](const json& jsFull) -> json
        {
            return {{"id", jsFull.value("id", "")}, {"version", jsFull.value("version", "")},
                    {"label", jsFull.value("label", "")}, {"description", jsFull.value("description", "")},
                    {"tags", jsFull.value("tags", json::object())}};
        };

        //locate the addressed resource: kind + config key prefix + builder.
        //source and flow annotations live on their sender entry (source_* /
        //flow_* fields) - they share its lifetime like the is-04 resources do
        std::string sKind, sPrefix, sField;
        size_t nIndex = 0;
        auto Resolve = [&]() -> bool
        {
            const std::string sBase = "/x-nmos/annotation/v1.0/node";
            if(sUri == sBase + "/self")    { sKind = "self";   sPrefix = "device"; return true; }
            if(sUri.rfind(sBase + "/devices/", 0) == 0 && sUri.substr(sBase.size()+9) == pNode->DeviceId())
            {
                sKind = "device"; sPrefix = "device"; return true;
            }
            for(size_t n = 0; n < pNode->SenderCount(); n++)
            {
                auto sSenders = "senders." + std::to_string(n);
                if(sUri == sBase + "/senders/" + pNode->SenderId(n))
                {
                    sKind = "sender"; sPrefix = sSenders; nIndex = n; return true;
                }
                if(sUri == sBase + "/sources/" + pNode->SourceId(n))
                {
                    sKind = "source"; sPrefix = sSenders; sField = "source_"; nIndex = n; return true;
                }
                if(sUri == sBase + "/flows/" + pNode->FlowId(n))
                {
                    sKind = "flow"; sPrefix = sSenders; sField = "flow_"; nIndex = n; return true;
                }
            }
            for(size_t n = 0; n < pNode->ReceiverCount(); n++)
            {
                if(sUri == sBase + "/receivers/" + pNode->ReceiverId(n))
                {
                    sKind = "receiver"; sPrefix = "receivers." + std::to_string(n); nIndex = n; return true;
                }
            }
            return false;
        };
        auto Build = [&]() -> json
        {
            if(sKind == "self")   { return View(pNode->BuildSelf()); }
            if(sKind == "device") { return View(pNode->BuildDevice()); }
            if(sKind == "sender") { return View(pNode->BuildSender(nIndex)); }
            if(sKind == "source") { return View(pNode->BuildSource(nIndex)); }
            if(sKind == "flow")   { return View(pNode->BuildFlow(nIndex)); }
            return View(pNode->BuildReceiver(nIndex));
        };

        //directory listings; resource lists are "<id>/" strings (resource-list.json)
        if(sUri == "/x-nmos/annotation")           { SendJsonCors(pConn, 200, json::array({"v1.0/"}), METHODS_RO); return 200; }
        if(sUri == "/x-nmos/annotation/v1.0")      { SendJsonCors(pConn, 200, json::array({"node/"}), METHODS_RO); return 200; }
        if(sUri == "/x-nmos/annotation/v1.0/node") { SendJsonCors(pConn, 200, json::array({"self/", "sources/", "flows/", "devices/", "senders/", "receivers/"}), METHODS_RO); return 200; }
        auto IdList = [&](const char* sWhat) -> json
        {
            json js = json::array();
            if(std::string(sWhat) == "devices") { js.push_back(pNode->DeviceId() + "/"); }
            for(size_t n = 0; n < pNode->SenderCount(); n++)
            {
                if(std::string(sWhat) == "senders") { js.push_back(pNode->SenderId(n) + "/"); }
                if(std::string(sWhat) == "sources") { js.push_back(pNode->SourceId(n) + "/"); }
                if(std::string(sWhat) == "flows")   { js.push_back(pNode->FlowId(n) + "/"); }
            }
            for(size_t n = 0; n < pNode->ReceiverCount(); n++)
            {
                if(std::string(sWhat) == "receivers") { js.push_back(pNode->ReceiverId(n) + "/"); }
            }
            return js;
        };
        for(const char* sWhat : {"devices", "senders", "receivers", "sources", "flows"})
        {
            if(sUri == std::string("/x-nmos/annotation/v1.0/node/") + sWhat)
            {
                SendJsonCors(pConn, 200, IdList(sWhat), METHODS_RO);
                return 200;
            }
        }

        if(!Resolve())
        {
            SendJson(pConn, 404, {{"code", 404}, {"error", "not found"}, {"debug", nullptr}});
            return 200;
        }
        if(sMethod == "GET" || sMethod == "HEAD")
        {
            SendJsonCors(pConn, 200, Build(), METHODS_RW);
            return 200;
        }
        if(sMethod != "PATCH")
        {
            SendJsonCors(pConn, 405, {{"code", 405}, {"error", "method not allowed"}, {"debug", nullptr}}, METHODS_RW);
            return 200;
        }

        std::string sBody;
        char buffer[8192];
        int nRead;
        while((nRead = mg_read(pConn, buffer, sizeof(buffer))) > 0) { sBody.append(buffer, size_t(nRead)); }
        json jsPatch = json::parse(sBody, nullptr, false);
        if(jsPatch.is_discarded() || !jsPatch.is_object())
        {
            SendJsonCors(pConn, 400, {{"code", 400}, {"error", "body must be a json object"}, {"debug", nullptr}}, METHODS_RW);
            return 200;
        }
        for(const auto& [sKey, jsValue] : jsPatch.items())
        {
            if(sKey != "label" && sKey != "description" && sKey != "tags")
            {
                SendJsonCors(pConn, 400, {{"code", 400}, {"error", "unknown field: " + sKey}, {"debug", nullptr}}, METHODS_RW);
                return 200;
            }
        }
        for(const char* sFieldName : {"label", "description"})
        {
            if(!jsPatch.contains(sFieldName)) { continue; }
            if(!jsPatch[sFieldName].is_string() && !jsPatch[sFieldName].is_null())
            {
                SendJsonCors(pConn, 400, {{"code", 400}, {"error", std::string(sFieldName) + " must be string or null"}, {"debug", nullptr}}, METHODS_RW);
                return 200;
            }
            //null resets to the default - builders fall back on empty values
            Config::Get().SetValue(sPrefix + "." + sField + sFieldName,
                                   jsPatch[sFieldName].is_null() ? "" : jsPatch[sFieldName].get<std::string>());
        }
        if(jsPatch.contains("tags"))
        {
            if(!jsPatch["tags"].is_object() && !jsPatch["tags"].is_null())
            {
                SendJsonCors(pConn, 400, {{"code", 400}, {"error", "tags must be object or null"}, {"debug", nullptr}}, METHODS_RW);
                return 200;
            }
            //per the patch schema: every tag is an array of STRINGS (or null =
            //delete that tag); tags null resets the whole set. keys merge into
            //the stored tags rather than replacing them wholesale.
            json jsTags = Config::Get().GetValue<json>(sPrefix + "." + sField + "tags", json::object());
            if(!jsTags.is_object()) { jsTags = json::object(); }
            if(jsPatch["tags"].is_null())
            {
                jsTags = json::object();
            }
            else
            {
                for(const auto& [sTag, jsValues] : jsPatch["tags"].items())
                {
                    if(IsReadOnlyTag(sTag))
                    {
                        //bcp-002 group hints and asset info are not user tags
                        SendJsonCors(pConn, 500, {{"code", 500}, {"error", "tag " + sTag + " is read-only"}, {"debug", nullptr}}, METHODS_RW);
                        return 200;
                    }
                    if(jsValues.is_null()) { jsTags.erase(sTag); continue; }
                    if(!jsValues.is_array())
                    {
                        SendJsonCors(pConn, 400, {{"code", 400}, {"error", "tag values must be an array of strings"}, {"debug", nullptr}}, METHODS_RW);
                        return 200;
                    }
                    for(const auto& jsItem : jsValues)
                    {
                        if(!jsItem.is_string())
                        {
                            SendJsonCors(pConn, 400, {{"code", 400}, {"error", "tag values must be strings"}, {"debug", nullptr}}, METHODS_RW);
                            return 200;
                        }
                    }
                    jsTags[sTag] = jsValues;
                }
            }
            Config::Get().SetValue(sPrefix + "." + sField + "tags", jsTags);
        }
        pNode->BumpVersion();   //re-registers the changed resources
        LOG_INFO("nmos") << "annotation patch on " << sKind << " - " << sBody.substr(0, 200);
        SendJsonCors(pConn, 200, Build(), METHODS_RW);
        return 200;
    }, this);
}

json NmosNode::GetStatusJson() const
{
    json jsConnected;
    json jsConnections = json::array();
    if(m_pConnection)
    {
        jsConnected = m_pConnection->ReceiverSenderId(0);
        for(size_t n = 0; n < m_vReceiverIds.size(); n++)
        {
            jsConnections.push_back(m_pConnection->ReceiverSenderId(n));
        }
    }
    std::lock_guard<std::mutex> lg(m_mutex);
    return {
        {"node_id", m_sNodeId},
        {"receiver_id", m_vReceiverIds.empty() ? "" : m_vReceiverIds[0]},
        {"sender_id", m_vSenderIds.empty() ? "" : m_vSenderIds[0]},
        {"receiver_ids", m_vReceiverIds},
        {"sender_ids", m_vSenderIds},
        {"connected_sender_id", jsConnected},
        {"connected_sender_ids", jsConnections},
        {"registry", m_sRegistry},
        {"status", m_sRegistryStatus},
        {"port", m_nPort}
    };
}
