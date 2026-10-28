#include "is12.h"
#include "civetweb.h"
#include "log.h"
#include "node.h"

using namespace pan::nmos;
using json = nlohmann::json;

namespace
{
    constexpr int MT_COMMAND = 0;
    constexpr int MT_COMMAND_RESPONSE = 1;
    constexpr int MT_NOTIFICATION = 2;
    constexpr int MT_SUBSCRIPTION = 3;
    constexpr int MT_SUBSCRIPTION_RESPONSE = 4;
    constexpr int MT_ERROR = 5;

    json Ok(const json& jsValue) { return {{"status", 200}, {"value", jsValue}}; }
    json OkVoid() { return {{"status", 200}}; }
    json Error(int nStatus, const std::string& sMessage) { return {{"status", nStatus}, {"errorMessage", sMessage}}; }

    json Counters(const std::map<std::string, uint64_t>& mCounters)
    {
        json js = json::array();
        for(const auto& [sName, nValue] : mCounters)
        {
            js.push_back({{"name", sName}, {"value", nValue}, {"description", nullptr}});
        }
        return js;
    }

    //property index base per domain: link 1-3, path 4-6, sync 7-9(+10 source), stream 11-13
    constexpr int DOMAIN_BASE[] = {1, 4, 7, 11};
}

Is12Server::Is12Server(NmosNode& node) : m_node(node)
{
    int nOid = 10;
    m_nReceivers = node.ReceiverCount();
    for(size_t n = 0; n < node.ReceiverCount(); n++)
    {
        auto sIndex = std::to_string(n+1);
        m_vMonitors.push_back({nOid++, "ReceiverMonitor" + sIndex, "receiver monitor " + sIndex,
                               "receiver", node.ReceiverId(n), {}, 0, "", std::nullopt, {}, {}, true});
        m_vMonitors.back().aDomains[LINK].nStatus = 1;
    }
    for(size_t n = 0; n < node.SenderCount(); n++)
    {
        auto sIndex = std::to_string(n+1);
        m_vMonitors.push_back({nOid++, "SenderMonitor" + sIndex, "sender monitor " + sIndex,
                               "sender", node.SenderId(n), {}, 0, "", std::nullopt, {}, {}, false});
        m_vMonitors.back().aDomains[LINK].nStatus = 1;
    }
}

Is12Server::Monitor* Is12Server::RxMonitor(size_t nStream)
{
    return nStream < m_nReceivers ? &m_vMonitors[nStream] : nullptr;
}

Is12Server::Monitor* Is12Server::TxMonitor(size_t nStream)
{
    size_t nIndex = m_nReceivers + nStream;
    return nIndex < m_vMonitors.size() ? &m_vMonitors[nIndex] : nullptr;
}

void Is12Server::Register(mg_context* pServer)
{
    mg_set_websocket_handler(pServer, "/x-nmos/ncp/v1.0", WsConnect, WsReady, WsData, WsClose, this);
}

int Is12Server::WsConnect(const mg_connection*, void*) { return 0; }

void Is12Server::WsReady(mg_connection* pConn, void* pUser)
{
    auto* pThis = static_cast<Is12Server*>(pUser);
    std::lock_guard<std::mutex> lg(pThis->m_mutex);
    pThis->m_mSubscriptions[pConn] = {};
    LOG_INFO("is12") << "controller connected";
}

void Is12Server::WsClose(const mg_connection* pConn, void* pUser)
{
    auto* pThis = static_cast<Is12Server*>(pUser);
    std::lock_guard<std::mutex> lg(pThis->m_mutex);
    pThis->m_mSubscriptions.erase(const_cast<mg_connection*>(pConn));
}

int Is12Server::WsData(mg_connection* pConn, int nBits, char* pData, size_t nSize, void* pUser)
{
    if((nBits & 0x0f) != MG_WEBSOCKET_OPCODE_TEXT) { return 1; }
    auto* pThis = static_cast<Is12Server*>(pUser);
    try
    {
        pThis->HandleMessage(pConn, json::parse(std::string(pData, nSize)));
    }
    catch(const std::exception& e)
    {
        pThis->SendTo(pConn, {{"messageType", MT_ERROR}, {"status", 400}, {"errorMessage", e.what()}});
    }
    return 1;
}

void Is12Server::SendTo(mg_connection* pConn, const json& jsMessage)
{
    auto sMessage = jsMessage.dump();
    mg_lock_connection(pConn);
    mg_websocket_write(pConn, MG_WEBSOCKET_OPCODE_TEXT, sMessage.data(), sMessage.size());
    mg_unlock_connection(pConn);
}

json Is12Server::MemberDescriptor(int nOid, const std::string& sRole, const std::string& sLabel,
                                  const std::vector<int>& vClassId) const
{
    return {{"description", nullptr}, {"role", sRole}, {"oid", nOid}, {"constantOid", true},
            {"classId", vClassId}, {"userLabel", sLabel}, {"owner", nOid == 1 ? json() : json(1)}};
}

Is12Server::Monitor* Is12Server::FindMonitor(int nOid)
{
    for(auto& monitor : m_vMonitors)
    {
        if(monitor.nOid == nOid) { return &monitor; }
    }
    return nullptr;
}

void Is12Server::HandleMessage(mg_connection* pConn, const json& jsMessage)
{
    int nType = jsMessage.value("messageType", -1);

    if(nType == MT_SUBSCRIPTION)
    {
        std::set<int> setOids;
        json jsAccepted = json::array();
        for(const auto& jsOid : jsMessage.value("subscriptions", json::array()))
        {
            if(jsOid.is_number())
            {
                int nOid = jsOid.get<int>();
                if(nOid == 1 || FindMonitor(nOid)) { setOids.insert(nOid); jsAccepted.push_back(nOid); }
            }
        }
        {
            std::lock_guard<std::mutex> lg(m_mutex);
            m_mSubscriptions[pConn] = setOids;
        }
        SendTo(pConn, {{"messageType", MT_SUBSCRIPTION_RESPONSE}, {"subscriptions", jsAccepted}});
        return;
    }

    if(nType != MT_COMMAND)
    {
        SendTo(pConn, {{"messageType", MT_ERROR}, {"status", 400}, {"errorMessage", "unsupported messageType"}});
        return;
    }

    json jsResponses = json::array();
    for(const auto& jsCommand : jsMessage.value("commands", json::array()))
    {
        json jsResult;
        int nOid = jsCommand.value("oid", -1);
        int nLevel = jsCommand.contains("methodId") ? jsCommand["methodId"].value("level", -1) : -1;
        int nIndex = jsCommand.contains("methodId") ? jsCommand["methodId"].value("index", -1) : -1;
        const json jsArgs = jsCommand.value("arguments", json::object());

        std::lock_guard<std::mutex> lg(m_mutex);
        if(nLevel == 1 && nIndex == 1)          //NcObject.Get
        {
            jsResult = GetProperty(nOid, jsArgs["id"].value("level", -1), jsArgs["id"].value("index", -1));
        }
        else if(nLevel == 1 && nIndex == 2)     //NcObject.Set - everything here is read only
        {
            jsResult = Error(405, "property is read only");
        }
        else if(nLevel == 2 && nIndex == 1 && nOid == 1)    //NcBlock.GetMemberDescriptors
        {
            json jsMembers = json::array({
                MemberDescriptor(2, "DeviceManager", "device manager", {1,3,1}),
                MemberDescriptor(3, "ClassManager", "class manager", {1,3,2})
            });
            for(const auto& monitor : m_vMonitors)
            {
                jsMembers.push_back(MemberDescriptor(monitor.nOid, monitor.sRole, monitor.sLabel,
                                                     monitor.bIsReceiver ? std::vector<int>{1,2,2,1} : std::vector<int>{1,2,2,2}));
            }
            jsResult = Ok(jsMembers);
        }
        else if(auto* pMonitor = FindMonitor(nOid); pMonitor && nLevel == 4)
        {
            jsResult = InvokeMethod(pMonitor, nOid, nLevel, nIndex, jsArgs);
        }
        else
        {
            jsResult = Error(501, "method not implemented");
        }

        jsResponses.push_back({{"handle", jsCommand.value("handle", 0)}, {"result", jsResult}});
    }
    SendTo(pConn, {{"messageType", MT_COMMAND_RESPONSE}, {"responses", jsResponses}});
}

json Is12Server::InvokeMethod(Monitor* pMonitor, int nOid, int nLevel, int nIndex, const json&)
{
    //receiver: 4m1 lost, 4m2 late, 4m3 reset / sender: 4m1 errors, 4m2 reset
    if(pMonitor->bIsReceiver)
    {
        if(nIndex == 1) { return Ok(Counters(pMonitor->mCounters1)); }
        if(nIndex == 2) { return Ok(Counters(pMonitor->mCounters2)); }
        if(nIndex == 3)
        {
            pMonitor->mCounters1.clear();
            pMonitor->mCounters2.clear();
            for(auto& domain : pMonitor->aDomains) { domain.nTransitions = 0; domain.sMessage.clear(); }
            return OkVoid();
        }
    }
    else
    {
        if(nIndex == 1) { return Ok(Counters(pMonitor->mCounters1)); }
        if(nIndex == 2)
        {
            pMonitor->mCounters1.clear();
            for(auto& domain : pMonitor->aDomains) { domain.nTransitions = 0; domain.sMessage.clear(); }
            return OkVoid();
        }
    }
    return Error(501, "method not implemented");
}

json Is12Server::GetProperty(int nOid, int nLevel, int nIndex)
{
    //caller holds m_mutex
    if(nOid == 1)   //root block
    {
        if(nLevel == 1)
        {
            switch(nIndex)
            {
                case 1: return Ok(json::array({1,1}));
                case 2: return Ok(1);
                case 3: return Ok(true);
                case 4: return Ok(nullptr);
                case 5: return Ok("root");
                case 6: return Ok("root");
                case 7: return Ok(json::array());
            }
        }
        if(nLevel == 2 && nIndex == 1)      //NcBlock.members
        {
            json jsMembers = json::array({
                MemberDescriptor(2, "DeviceManager", "device manager", {1,3,1}),
                MemberDescriptor(3, "ClassManager", "class manager", {1,3,2})
            });
            for(const auto& monitor : m_vMonitors)
            {
                jsMembers.push_back(MemberDescriptor(monitor.nOid, monitor.sRole, monitor.sLabel,
                                                     monitor.bIsReceiver ? std::vector<int>{1,2,2,1} : std::vector<int>{1,2,2,2}));
            }
            return Ok(jsMembers);
        }
        return Error(502, "property not implemented");
    }
    if(nOid == 2)   //device manager
    {
        if(nLevel == 1)
        {
            switch(nIndex)
            {
                case 1: return Ok(json::array({1,3,1}));
                case 2: return Ok(2);
                case 5: return Ok("DeviceManager");
                case 6: return Ok("device manager");
                case 7: return Ok(json::array());
            }
        }
        if(nLevel == 3)
        {
            switch(nIndex)
            {
                case 1: return Ok("v1.0.0");
                case 2: return Ok({{"name", "AES67eleven"}, {"organizationId", nullptr}, {"website", nullptr}});
                case 3: return Ok({{"name", "AES67eleven"}, {"key", "aes67eleven"}, {"revisionLevel", APP_VERSION},
                                   {"brandName", nullptr}, {"uuid", nullptr}, {"description", nullptr}});
                case 4: return Ok(m_node.NodeId());
                case 8: return Ok({{"generic", 1}, {"deviceSpecificDetails", nullptr}});
                case 9: return Ok(0);
                default: return Ok(nullptr);
            }
        }
        return Error(502, "property not implemented");
    }
    if(nOid == 3)   //class manager
    {
        if(nLevel == 1)
        {
            switch(nIndex)
            {
                case 1: return Ok(json::array({1,3,2}));
                case 2: return Ok(3);
                case 5: return Ok("ClassManager");
                case 6: return Ok("class manager");
                case 7: return Ok(json::array());
            }
        }
        if(nLevel == 3 && (nIndex == 1 || nIndex == 2)) { return Ok(json::array()); }
        return Error(502, "property not implemented");
    }

    auto* pMonitor = FindMonitor(nOid);
    if(!pMonitor) { return Error(404, "unknown oid"); }

    if(nLevel == 1)
    {
        switch(nIndex)
        {
            case 1: return Ok(pMonitor->bIsReceiver ? json::array({1,2,2,1}) : json::array({1,2,2,2}));
            case 2: return Ok(pMonitor->nOid);
            case 3: return Ok(true);
            case 4: return Ok(1);
            case 5: return Ok(pMonitor->sRole);
            case 6: return Ok(pMonitor->sLabel);
            case 7: return Ok(json::array({{{"contextNamespace", "x-nmos"},
                        {"resource", {{"resourceType", pMonitor->sTouchpointType}, {"id", pMonitor->sTouchpointId}}}}}));
        }
        return Error(502, "property not implemented");
    }
    if(nLevel == 2 && nIndex == 1) { return Ok(true); }     //NcWorker.enabled
    if(nLevel == 3)
    {
        switch(nIndex)
        {
            case 1: return Ok(pMonitor->nOverall);
            case 2: return Ok(pMonitor->sOverallMessage);
            case 3: return Ok(3);   //statusReportingDelay
        }
        return Error(502, "property not implemented");
    }
    if(nLevel == 4)
    {
        for(int nDomain = 0; nDomain < 4; nDomain++)
        {
            int nBase = DOMAIN_BASE[nDomain];
            if(nIndex == nBase)   { return Ok(pMonitor->aDomains[nDomain].nStatus); }
            if(nIndex == nBase+1) { return Ok(pMonitor->aDomains[nDomain].sMessage); }
            if(nIndex == nBase+2) { return Ok(pMonitor->aDomains[nDomain].nTransitions); }
        }
        if(nIndex == 10) { return Ok(pMonitor->sSyncSource ? json(*pMonitor->sSyncSource) : json()); }
        if(nIndex == 14) { return Ok(true); }   //autoResetCountersAndMessages
        return Error(502, "property not implemented");
    }
    return Error(502, "property not implemented");
}

void Is12Server::Notify(int nOid, int nLevel, int nIndex, const json& jsValue)
{
    //caller holds m_mutex
    json jsNotification = {
        {"messageType", MT_NOTIFICATION},
        {"notifications", json::array({{
            {"oid", nOid},
            {"eventId", {{"level", 1}, {"index", 1}}},
            {"eventData", {{"propertyId", {{"level", nLevel}, {"index", nIndex}}},
                           {"changeType", 0}, {"value", jsValue}, {"sequenceItemIndex", nullptr}}}
        }})}
    };
    for(const auto& [pConn, setOids] : m_mSubscriptions)
    {
        if(setOids.contains(nOid))
        {
            SendTo(pConn, jsNotification);
        }
    }
}

void Is12Server::UpdateDomain(Monitor& monitor, int nDomain, int nStatus, const std::string& sMessage)
{
    //caller holds m_mutex. statusReportingDelay (3s): a state less healthy than
    //the current one is only reported once it has persisted for the delay, so
    //transients right after a switch never show up. healthier states apply
    //immediately.
    auto& domain = monitor.aDomains[nDomain];
    if(nStatus > 1 && nStatus > domain.nStatus)
    {
        auto now = std::chrono::steady_clock::now();
        if(domain.nPendingStatus != nStatus)
        {
            domain.nPendingStatus = nStatus;
            domain.pendingSince = now;
        }
        if(now - domain.pendingSince < std::chrono::seconds(3)) { return; }
    }
    domain.nPendingStatus = -1;
    int nBase = DOMAIN_BASE[nDomain];
    if(domain.nStatus != nStatus)
    {
        if(nStatus > domain.nStatus && domain.nStatus != 0)
        {
            domain.nTransitions++;
            Notify(monitor.nOid, 4, nBase+2, domain.nTransitions);
        }
        domain.nStatus = nStatus;
        Notify(monitor.nOid, 4, nBase, nStatus);
    }
    if(domain.sMessage != sMessage)
    {
        domain.sMessage = sMessage;
        Notify(monitor.nOid, 4, nBase+1, sMessage);
    }
    RecomputeOverall(monitor);
}

void Is12Server::RecomputeOverall(Monitor& monitor)
{
    //caller holds m_mutex. overall = inactive while the path domain is inactive,
    //else the worst of all active domains
    int nOverall;
    std::string sMessage;
    if(monitor.aDomains[PATH].nStatus == 0)
    {
        nOverall = 0;
    }
    else
    {
        nOverall = monitor.aDomains[PATH].nStatus;
        sMessage = monitor.aDomains[PATH].sMessage;
        for(int nDomain : {LINK, SYNC, STREAM})
        {
            const auto& domain = monitor.aDomains[nDomain];
            if(nDomain == SYNC && domain.nStatus == 0) { continue; }    //sync 0 = not used
            if(nDomain == STREAM && domain.nStatus == 0) { continue; }
            if(domain.nStatus > nOverall)
            {
                nOverall = domain.nStatus;
                sMessage = domain.sMessage;
            }
        }
    }
    if(nOverall != monitor.nOverall)
    {
        monitor.nOverall = nOverall;
        Notify(monitor.nOid, 3, 1, nOverall);
    }
    if(sMessage != monitor.sOverallMessage)
    {
        monitor.sOverallMessage = sMessage;
        Notify(monitor.nOid, 3, 2, sMessage);
    }
}

void Is12Server::UpdateReceiverDomain(size_t nStream, int nDomain, int nStatus, const std::string& sMessage)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(auto* pMonitor = RxMonitor(nStream)) { UpdateDomain(*pMonitor, nDomain, nStatus, sMessage); }
}

void Is12Server::UpdateSenderDomain(size_t nStream, int nDomain, int nStatus, const std::string& sMessage)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(auto* pMonitor = TxMonitor(nStream)) { UpdateDomain(*pMonitor, nDomain, nStatus, sMessage); }
}

void Is12Server::SetSyncSource(const std::optional<std::string>& sSourceId)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    for(auto& monitor : m_vMonitors)
    {
        if(monitor.sSyncSource != sSourceId)
        {
            monitor.sSyncSource = sSourceId;
            Notify(monitor.nOid, 4, 10, sSourceId ? json(*sSourceId) : json());
        }
    }
}

void Is12Server::AddReceiverLost(size_t nStream, const std::string& sLeg, uint64_t nPackets)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(auto* pMonitor = RxMonitor(nStream)) { pMonitor->mCounters1[sLeg] += nPackets; }
}

void Is12Server::AddReceiverLate(size_t nStream, uint64_t nPackets)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(auto* pMonitor = RxMonitor(nStream)) { pMonitor->mCounters2["late"] += nPackets; }
}

void Is12Server::AddSenderErrors(size_t nStream, const std::string& sCounter, uint64_t nErrors)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(auto* pMonitor = TxMonitor(nStream)) { pMonitor->mCounters1[sCounter] += nErrors; }
}

json Is12Server::GetMonitorJson() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    auto Dump = [](const Monitor& monitor)
    {
        const char* DOMAINS[] = {"link", "path", "sync", "stream"};
        json js = {{"overall", monitor.nOverall}, {"overall_message", monitor.sOverallMessage}};
        for(int nDomain = 0; nDomain < 4; nDomain++)
        {
            js[DOMAINS[nDomain]] = {{"status", monitor.aDomains[nDomain].nStatus},
                                    {"message", monitor.aDomains[nDomain].sMessage},
                                    {"transitions", monitor.aDomains[nDomain].nTransitions}};
        }
        js["counters"] = Counters(monitor.mCounters1);
        js["counters2"] = Counters(monitor.mCounters2);
        return js;
    };
    json js;
    js["receivers"] = json::array();
    js["senders"] = json::array();
    for(const auto& monitor : m_vMonitors)
    {
        js[monitor.bIsReceiver ? "receivers" : "senders"].push_back(Dump(monitor));
    }
    //stream 0 aliases keep the existing web ui working
    if(!js["receivers"].empty()) { js["receiver"] = js["receivers"][0]; }
    if(!js["senders"].empty())   { js["sender"] = js["senders"][0]; }
    return js;
}
