#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "audio/sources.h"
#include "bridge/rx_bridge.h"
#include "bridge/tx_bridge.h"
#include "compat/net_compat.h"
#include "compat/time_compat.h"
#include "config.h"
#include "coreaudio/channel_meters.h"
#ifdef __APPLE__
#include "coreaudio/hal_device.h"
#include "coreaudio/monitor_out.h"
#else
#include "linux/hal_device.h"
#include "linux/monitor_out.h"
#endif
#include "driver_iface.h"
#include "player.h"
#include "log.h"
#ifdef __APPLE__
#include "menubar.h"
#else
#include "linux/menubar.h"
#endif
#include "nmos/connection.h"
#include "nmos/is12.h"
#include "nmos/node.h"
#include "ptp/ptpclient.h"
#include "rtp/rtpreceiver.h"
#include "rtp/rtpsender.h"
#include "web/server.h"

using namespace pan;
using namespace aes67vsc;
using json = nlohmann::json;

#ifdef __APPLE__
constexpr bool kGatewayEngine = false;  //coreaudio tap paces the engine
#else
constexpr bool kGatewayEngine = true;   //no device - the engine paces itself
#endif

namespace
{
    volatile sig_atomic_t g_bStop = 0;
    volatile sig_atomic_t g_bRestart = 0;
    char** g_argv = nullptr;
    void OnSignal(int) { g_bStop = 1; }

    std::vector<std::string> Interfaces()
    {
        std::vector<std::string> vInterfaces{Config::Get().GetValue<std::string>("network.interface_primary", "en0")};
        auto sSecondary = Config::Get().GetValue<std::string>("network.interface_secondary", "");
        if(!sSecondary.empty() && sSecondary != vInterfaces[0]) { vInterfaces.push_back(sSecondary); }
        return vInterfaces;
    }

    std::string HomePath(const std::string& sSuffix)
    {
        const char* sHome = getenv("HOME");
        return std::string(sHome ? sHome : "/tmp") + sSuffix;
    }

    std::string RandomUid()
    {
        std::random_device rd;
        char buffer[20];
        snprintf(buffer, sizeof(buffer), "%08x%08x", rd(), rd());
        return buffer;
    }

    json StreamArray(const char* sKey)
    {
        auto js = Config::Get().GetJson();
        return js.contains(sKey) && js[sKey].is_array() ? js[sKey] : json::array();
    }

    /** the channel map routes stream channels onto the 64ch device matrix;
    *   without an explicit map the contiguous offset+channels block applies **/
    std::vector<uint32_t> MapFromEntry(const json& jsEntry, size_t nIndex)
    {
        std::vector<uint32_t> vMap;
        if(jsEntry.contains("channel_map") && jsEntry["channel_map"].is_array())
        {
            for(const auto& jsCh : jsEntry["channel_map"])
            {
                if(jsCh.is_number()) { vMap.push_back(uint32_t(std::max(0, jsCh.get<int>()))); }
            }
        }
        if(vMap.empty())
        {
            auto nOffset = size_t(std::max(0, jsEntry.value("channel_offset", int(nIndex)*8)));
            auto nCount = size_t(std::clamp(jsEntry.value("channels", 8), 1, int(kMaxStreamChannels)));
            for(size_t nCh = 0; nCh < nCount; nCh++) { vMap.push_back(uint32_t(nOffset + nCh)); }
        }
        return vMap;
    }

    /** non-owning adapter so a sender can "own" the shared device tap **/
    class SourceRef : public audio::Source
    {
        public:
            explicit SourceRef(audio::Source& source) : m_source(source) {}
            bool Read(float* pInterleaved, size_t nFrames) override { return m_source.Read(pInterleaved, nFrames); }
            std::string Describe() const override { return m_source.Describe(); }
            size_t Channels() const override { return m_source.Channels(); }
        private:
            audio::Source& m_source;
    };

    /** pre-v2 configs held a single "sender" object - move it onto slot 0;
    *   ensure every stream entry carries a persistent uid **/
    void MigrateConfig()
    {
        auto js = Config::Get().GetJson();
        if(js.contains("sender") && js["sender"].is_object())
        {
            json jsUpdates = json::object();
            for(const auto& [sKey, jsValue] : js["sender"].items())
            {
                jsUpdates["senders.0." + sKey] = jsValue;
            }
            Config::Get().Update(jsUpdates);
            Config::Get().Remove("sender");
            LOG_INFO("main") << "migrated single-sender config onto senders[0]";
        }
        //uuid seed: nmos ids derive from it. freeze the current hostname once,
        //so dhcp renaming the mac at another site can never change our ids
        if(Config::Get().GetValue<std::string>("device.uuid_seed", "").empty())
        {
            char sHost[128] = {0};
            gethostname(sHost, sizeof(sHost)-1);
            Config::Get().SetValue("device.uuid_seed", std::string(sHost));
            LOG_INFO("main") << "uuid seed frozen: " << sHost;
        }
        for(const char* sKey : {"senders", "receivers"})
        {
            auto jsStreams = StreamArray(sKey);
            bool bChanged = false;
            for(size_t n = 0; n < jsStreams.size(); n++)
            {
                if(jsStreams[n].value("uid", "").empty())
                {
                    jsStreams[n]["uid"] = n == 0 ? "legacy" : RandomUid();
                    bChanged = true;
                }
            }
            if(bChanged) { Config::Get().SetValue(sKey, jsStreams); }
        }
    }

    void ApplySender(size_t n, rtp::RtpSender& sender, TxTap* pTap, const std::string& sFilesDir)
    {
        auto jsStreams = StreamArray("senders");
        json js = n < jsStreams.size() ? jsStreams[n] : json::object();
        if(!js.value("enabled", false))
        {
            sender.Stop();
            return;
        }
        auto vInterfaces = Interfaces();
        std::vector<rtp::RtpSender::Leg> vLegs{{vInterfaces[0],
                                                js.value("multicast_primary", "239.69.145." + std::to_string(10+n)),
                                                js.value("leg1_enabled", true)}};
        if(vInterfaces.size() > 1)
        {
            vLegs.push_back({vInterfaces[1],
                             js.value("multicast_secondary", "239.69.146." + std::to_string(10+n)),
                             js.value("leg2_enabled", true)});
        }

        //"device" streams the tap's channel pick of AES67 Out; the other
        //sources are built-in test signals
        std::unique_ptr<audio::Source> pSource;
        if(js.value("source", "device") == "device" && pTap)
        {
            pSource = std::make_unique<SourceRef>(*pTap);
        }
        else
        {
            pSource = audio::CreateFromConfig(js, sFilesDir);
        }
        int nChannels = pTap ? int(pTap->Channels()) : std::clamp(js.value("channels", 8), 1, int(kMaxStreamChannels));
        sender.Configure(vLegs, static_cast<uint16_t>(js.value("port", 5004)), js.value("payload_type", 96),
                         js.value("packet_time_us", 1000), nChannels,
                         js.value("bits", 24), js.value("label", ""),
                         js.value("multicast_loop", true), std::move(pSource));
    }
}

int main(int argc, char** argv)
{
    g_argv = argv;
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGPIPE, SIG_IGN);

    //finder double-click while the agent already runs: just raise the web ui.
    //(launchd starts us WITH a config argument, interactive starts without)
    if(argc <= 1)
    {
        int nProbe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in probeAddr{};
        probeAddr.sin_family = AF_INET;
        probeAddr.sin_port = htons(8180);
        inet_pton(AF_INET, "127.0.0.1", &probeAddr.sin_addr);
        timeval tv{0, 300000};
        setsockopt(nProbe, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        bool bRunning = connect(nProbe, reinterpret_cast<sockaddr*>(&probeAddr), sizeof(probeAddr)) == 0;
        close(nProbe);
        if(bRunning)
        {
            (void)!system("open http://localhost:8180");
            return 0;
        }
    }

    auto sSupportDir = HomePath("/Library/Application Support/aes67-vsc");
    mkdir(sSupportDir.c_str(), 0755);
    std::string sConfig = argc > 1 ? argv[1] : sSupportDir + "/config.json";
    Config::Get().Load(sConfig);
    MigrateConfig();
    LOG_INFO("main") << "aes67-vsc-daemon " << APP_VERSION << " config " << sConfig;

    auto sFilesDir = Config::Get().GetValue<std::string>("web.audio_files_path", "");
    if(sFilesDir.empty()) { sFilesDir = sSupportDir + "/audio-files"; }
    mkdir(sFilesDir.c_str(), 0755);

    //---- file players (web ui "player" page, mixed onto the 64ch tx bus) ----
    std::array<PlayerDeck, 4> playerDecks;
    {
        auto jsPlayers = StreamArray("players");
        for(size_t n = 0; n < playerDecks.size() && n < jsPlayers.size(); n++)
        {
            playerDecks[n].Settings(jsPlayers[n]);
            auto sFile = jsPlayers[n].value("file", "");
            if(!sFile.empty() && sFile.find('/') == std::string::npos)
            {
                if(playerDecks[n].Load(sFilesDir + "/" + sFile, sFile) &&
                   jsPlayers[n].value("autostart", false))
                {
                    playerDecks[n].Control("play");
                }
            }
        }
    }
    auto PersistPlayers = [&]()
    {
        json jsPlayers = json::array();
        for(const auto& deck : playerDecks) { jsPlayers.push_back(deck.Persist()); }
        Config::Get().SetValue("players", jsPlayers);
    };

    //---- engine ----
    ptp::PtpClient ptpClient;
    ptpClient.SetAutoDomain(Config::Get().GetValue<bool>("ptp.domain_auto", true));
    ptpClient.Run(Interfaces(), Config::Get().GetValue<int>("ptp.domain", 0));

    TxFanout txFanout;
    RxMixer rxMixer;
    MonitorOut monitor;
    //the vectors are long-lived (NmosNode holds references); topology changes
    //refill them under the topology mutex while the nmos server is stopped
    std::vector<std::unique_ptr<rtp::RtpSender>> vSenders;
    std::vector<std::unique_ptr<rtp::RtpReceiver>> vReceivers;
    std::vector<std::shared_ptr<TxTap>> vTxTaps;
    std::vector<std::shared_ptr<RxTap>> vRxTaps;
    std::mutex topologyMutex;

    auto BuildStreams = [&]()
    {
        //caller holds topologyMutex and has stopped the previous streams
        vSenders.clear();
        vReceivers.clear();
        vTxTaps.clear();
        vRxTaps.clear();

        auto jsSenders = StreamArray("senders");
        auto jsReceivers = StreamArray("receivers");
        for(size_t n = 0; n < jsSenders.size() && n < kMaxStreams; n++)
        {
            vTxTaps.push_back(std::make_shared<TxTap>(MapFromEntry(jsSenders[n], n)));
            if(kGatewayEngine)
            {
                vTxTaps.back()->SetMaxFill(size_t(Config::Get().GetValue<double>("gateway.buffer_ms", 30.0)*48.0) + 448);
            }
            vSenders.push_back(std::make_unique<rtp::RtpSender>(ptpClient));
        }
        for(size_t n = 0; n < jsReceivers.size() && n < kMaxStreams; n++)
        {
            vRxTaps.push_back(std::make_shared<RxTap>(MapFromEntry(jsReceivers[n], n)));
            vReceivers.push_back(std::make_unique<rtp::RtpReceiver>(ptpClient, *vRxTaps.back()));
        }
        txFanout.SetTaps(vTxTaps);
        rxMixer.SetTaps(vRxTaps);

        if(!vReceivers.empty())
        {
            vReceivers[0]->SetMonitor(Config::Get().GetValue<int>("receiver.monitor_left", -1),
                                      Config::Get().GetValue<int>("receiver.monitor_right", -1));
        }
        for(size_t n = 0; n < vSenders.size(); n++)
        {
            ApplySender(n, *vSenders[n], vTxTaps[n].get(), sFilesDir);
        }
        LOG_INFO("main") << vSenders.size() << " senders + " << vReceivers.size()
                         << " receivers on " << kChannels << "ch devices";
    };

    auto ApplyMonitor = [&]()
    {
        //the monitor follows ONE receiver: it taps that stream's first two
        //mapped device channels out of the composed render
        auto nStream = size_t(std::max(0, Config::Get().GetValue<int>("monitor.stream", 0)));
        auto jsReceivers = StreamArray("receivers");
        int nLeft = 0, nRight = 1;
        if(nStream < jsReceivers.size())
        {
            auto vMap = MapFromEntry(jsReceivers[nStream], nStream);
            nLeft = int(vMap[0]);
            nRight = int(vMap.size() > 1 ? vMap[1] : vMap[0]);
        }
        monitor.SetChannels(nLeft, nRight);
        if(Config::Get().GetValue<bool>("monitor.enabled", false))
        {
            monitor.Start(Config::Get().GetValue<std::string>("monitor.device", "default"),
                          Config::Get().GetValue<double>("monitor.gain_db", 0.0));
        }
        else
        {
            monitor.Stop();
        }
    };
    rxMixer.SetMonitor(&monitor);
    ApplyMonitor();

    {
        std::lock_guard<std::mutex> lg(topologyMutex);
        BuildStreams();
    }

    //---- core audio bridge device ----
    //the hidden bridge is the daemon's window onto the visible "AES67" device:
    //its input carries what apps play (-> rtp tx), its output feeds what apps
    //record (<- rtp rx). one io proc serves both directions.
    HalDevice bridge;       //output only: renders received audio (no mic io)
    HalDevice mainDevice;   //property handle for the capture tap
    std::atomic<bool> bDevicesUp{false};
    ChannelMeters outMeters;    //what apps play onto "AES67" (towards the network)
    ChannelMeters inMeters;     //what apps record from "AES67" (from the network)

    auto OpenDevices = [&]() -> bool
    {
        bool bOk = bridge.Open(kDeviceUidBridge) &&
                   bridge.Start([&](const AudioBufferList*, AudioBufferList* pOutput)
                   {
                       if(pOutput && pOutput->mNumberBuffers > 0)
                       {
                           auto& buffer = pOutput->mBuffers[0];
                           size_t nFrames = buffer.mDataByteSize / (kChannels * sizeof(float));
                           rxMixer.Render(static_cast<float*>(buffer.mData), nFrames);
                           inMeters.Feed(static_cast<const float*>(buffer.mData), nFrames);
                       }
                   }, /*bOutputOnly*/ true);
        bOk = bOk && mainDevice.Open(kDeviceUidMain);
        bDevicesUp = bOk;
        if(!bDevicesUp)
        {
            static uint64_t nFailures = 0;      //the 1 Hz retry would spam the log
            if(nFailures++ % 30 == 0)
            {
                LOG_ERROR("main") << "bridge device not found (driver installed? coreaudiod restarted?)";
            }
        }
        else
        {
            LOG_INFO("main") << "attached to " << kDeviceUidBridge;
        }
        return bDevicesUp;
    };
    OpenDevices();

    //capture drain: pull what apps play on the main device out of the driver
    //via the capture tap property (a plain hal property read - not an input
    //stream, so macos never shows the microphone indicator for us)
    std::thread captureThread([&]()
    {
        //when apps play on the device, the driver tap paces us and the file
        //players are mixed into those frames. with no app running the tap is
        //silent, so the players are paced by a host-clock frame budget
        //(clamped so the tap stays the master whenever it delivers)
        auto tLast = std::chrono::steady_clock::now();
        //gateway: prime the tx rings with a scheduling cushion - rtp stamps are
        //taken at ring read time, so this dwell does not add link latency
        const double dPrime = kGatewayEngine ?
            Config::Get().GetValue<double>("gateway.buffer_ms", 30.0) * 48.0 : 0.0;
        double dBudget = dPrime;
        std::vector<float> vPlayerMix;
        if(kGatewayEngine) { compat::SetRealtimeThread(8'000'000); }
        uint64_t nNextNs = compat::HostNs();
        while(!g_bStop)
        {
            if(kGatewayEngine)
            {
                //absolute pacing: a late wakeup is caught up, not accumulated
                nNextNs += 8'000'000;
                auto nNow = compat::HostNs();
                if(nNow > nNextNs + 500'000'000) { nNextNs = nNow; }    //suspended? resync
                compat::SleepUntilHostNs(nNextNs);
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            auto tNow = std::chrono::steady_clock::now();
            dBudget += std::chrono::duration<double>(tNow - tLast).count() * kSampleRate;
            tLast = tNow;
            if(!bDevicesUp) { dBudget = 0.0; continue; }

            bool bPlayers = std::any_of(playerDecks.begin(), playerDecks.end(),
                                        [](const PlayerDeck& deck) { return deck.Playing(); });
            auto vBytes = mainDevice.GetDataProperty(kCaptureTapSelector);
            size_t nFrames = vBytes.size() / (kChannels * sizeof(float));
            if(nFrames > 0)
            {
                dBudget = std::clamp(dBudget - double(nFrames), -4096.0, 4096.0);
                auto* pFrames = reinterpret_cast<float*>(vBytes.data());
                for(auto& deck : playerDecks) { deck.Mix(pFrames, nFrames); }
                txFanout.Push(pFrames, nFrames);
                outMeters.Feed(pFrames, nFrames);
            }
            else if((bPlayers || kGatewayEngine) && dBudget >= 384.0)
            {
                dBudget = std::min(dBudget, dPrime + 9600.0);
                auto nMix = static_cast<size_t>(std::min(dBudget, 4096.0));
                vPlayerMix.assign(nMix * kChannels, 0.0f);
                for(auto& deck : playerDecks) { deck.Mix(vPlayerMix.data(), nMix); }
                txFanout.Push(vPlayerMix.data(), nMix);
                outMeters.Feed(vPlayerMix.data(), nMix);
                if(kGatewayEngine)
                {
                    //no bridge device renders the rx side here - pull the
                    //receivers at the same pace so buffers and meters live
                    static std::vector<float> vRxRender;
                    vRxRender.assign(nMix * kChannels, 0.0f);
                    rxMixer.Render(vRxRender.data(), nMix);
                    inMeters.Feed(vRxRender.data(), nMix);
                }
                dBudget -= double(nMix);
            }
            else if(!bPlayers && !kGatewayEngine)
            {
                dBudget = std::min(dBudget, 0.0);
            }
        }
    });

    std::atomic<bool> bReopenPending{false};
    OnAudioServiceRestarted([&]() { bReopenPending = true; });

    //(no microphone permission needed - capture goes through the driver's
    //tap property, the bridge is output-only, so no orange mic indicator)

    //---- nmos ----
    nmos::NmosNode node(ptpClient, vSenders, vReceivers);
    nmos::NmosNode::Callbacks callbacks;
    callbacks.onReceiver = [&](size_t n, bool bEnable, const std::optional<rtp::SdpSession>& session)
    {
        std::lock_guard<std::mutex> lg(topologyMutex);
        if(n >= vReceivers.size()) { return; }
        if(bEnable && session)
        {
            vRxTaps[n]->SetExpectAudio(true);
            vReceivers[n]->Configure(*session, Interfaces(),
                                     Config::Get().GetValue<int>("receiver.playout_delay_ms", 20),
                                     [](){ return Config::Get().GetValue<double>("receiver.gain_db", 0.0); });
        }
        else
        {
            vRxTaps[n]->SetExpectAudio(false);
            vReceivers[n]->Stop();
        }
    };
    callbacks.onSender = [&](size_t n, bool bEnable)
    {
        std::lock_guard<std::mutex> lg(topologyMutex);
        if(n >= vSenders.size()) { return; }
        Config::Get().SetValue("senders." + std::to_string(n) + ".enabled", bEnable);
        ApplySender(n, *vSenders[n], vTxTaps[n].get(), sFilesDir);
        node.BumpVersion();
    };
    bool bNmosUp = false;
    if(Config::Get().GetValue<bool>("nmos.enabled", true))
    {
        bNmosUp = node.Start(Config::Get().GetValue<int>("nmos.http_port", 8080), Interfaces(), callbacks);
    }

    //full topology rebuild: nmos server down -> streams rebuilt from config ->
    //nmos back up with fresh resources; orphaned resources are deleted from
    //the registry so controllers do not keep stale entries
    static std::mutex applyTopologyMutex;   //two rapid ui actions must not interleave
    auto ApplyTopology = [&]()
    {
        std::lock_guard<std::mutex> apply(applyTopologyMutex);
        auto sRegistry = node.GetStatusJson().value("registry", "");
        std::set<std::string> setOldIds;
        std::vector<std::pair<std::string, std::string>> vOld;  //type, id
        for(size_t n = 0; n < node.SenderCount(); n++)
        {
            vOld.push_back({"senders", node.SenderId(n)});
            setOldIds.insert(node.SenderId(n));
        }
        for(size_t n = 0; n < node.ReceiverCount(); n++)
        {
            vOld.push_back({"receivers", node.ReceiverId(n)});
            setOldIds.insert(node.ReceiverId(n));
        }

        node.Stop();
        {
            std::lock_guard<std::mutex> lg(topologyMutex);
            for(auto& pSender : vSenders) { pSender->Stop(); }
            for(auto& pTap : vRxTaps) { pTap->Shutdown(); }
            for(auto& pReceiver : vReceivers) { pReceiver->Stop(); }
            BuildStreams();
        }
        bNmosUp = false;
        if(Config::Get().GetValue<bool>("nmos.enabled", true))
        {
            bNmosUp = node.Start(Config::Get().GetValue<int>("nmos.http_port", 8080), Interfaces(), callbacks);
        }

        ApplyMonitor();     //the monitored receiver's channels may have moved

        //registry cleanup for removed streams - fire and forget, never block
        //the topology change on a slow/unreachable registry
        if(!sRegistry.empty() && bNmosUp)
        {
            std::set<std::string> setNewIds;
            for(size_t n = 0; n < node.SenderCount(); n++) { setNewIds.insert(node.SenderId(n)); }
            for(size_t n = 0; n < node.ReceiverCount(); n++) { setNewIds.insert(node.ReceiverId(n)); }
            std::vector<std::pair<std::string, std::string>> vRemove;
            for(const auto& [sType, sId] : vOld)
            {
                if(!setNewIds.contains(sId)) { vRemove.push_back({sType, sId}); }
            }
            if(!vRemove.empty())
            {
                std::thread([sRegistry, vRemove]()
                {
                    for(const auto& [sType, sId] : vRemove)
                    {
                        int nStatus = 0;
                        nmos::HttpJson("DELETE", sRegistry + "/x-nmos/registration/v1.3/resource/" + sType + "/" + sId,
                                       nullptr, nStatus);
                        LOG_INFO("main") << "unregistered removed " << sType << " " << sId << " -> " << nStatus;
                    }
                }).detach();
            }
        }
    };

    //---- web ui ----
    auto sWebRoot = Config::Get().GetValue<std::string>("web.webui_path", "webui");
    web::WebServer web;
    auto StatusJson = [&]() -> json
    {
        std::lock_guard<std::mutex> lg(topologyMutex);
        json js;
        js["version"] = APP_VERSION;
        uint64_t nUiStamp = 0;
        for(const char* sFile : {"index.html", "app.js", "style.css"})
        {
            struct stat st{};
            if(stat((sWebRoot + "/" + std::string(sFile)).c_str(), &st) == 0)
            {
                nUiStamp = std::max(nUiStamp, static_cast<uint64_t>(st.st_mtime));
            }
        }
        js["build"] = std::string(__DATE__ " " __TIME__ " ui-") + std::to_string(nUiStamp);
        js["ptp"] = ptpClient.GetStatusJson();
        //stream 0 keeps the existing ui panels alive, the arrays carry them all
        js["sender"] = vSenders.empty() ? json::object() : vSenders[0]->GetStatusJson();
        js["receiver"] = vReceivers.empty() ? json::object() : vReceivers[0]->GetStatusJson();
        js["receiver"]["gain_db"] = Config::Get().GetValue<double>("receiver.gain_db", 0.0);
        js["senders"] = json::array();
        js["receivers"] = json::array();
        for(size_t n = 0; n < vSenders.size(); n++)
        {
            auto jsSender = vSenders[n]->GetStatusJson();
            jsSender["index"] = n;
            jsSender["id"] = node.SenderCount() > n ? node.SenderId(n) : "";
            jsSender["channel_map"] = vTxTaps[n]->Map();
            jsSender["tap_fill"] = vTxTaps[n]->FillFrames();
            jsSender["tap_underruns"] = vTxTaps[n]->Underruns();
            jsSender["tap_trimmed"] = vTxTaps[n]->Trimmed();
            js["senders"].push_back(jsSender);
        }
        for(size_t n = 0; n < vReceivers.size(); n++)
        {
            auto jsReceiver = vReceivers[n]->GetStatusJson();
            jsReceiver["index"] = n;
            jsReceiver["id"] = node.ReceiverCount() > n ? node.ReceiverId(n) : "";
            jsReceiver["channel_map"] = vRxTaps[n]->Map();
            jsReceiver["tap_fill"] = vRxTaps[n]->DelayFrames();
            jsReceiver["tap_underruns"] = vRxTaps[n]->Underruns();
            js["receivers"].push_back(jsReceiver);
        }
        js["players"] = json::array();
        for(const auto& deck : playerDecks) { js["players"].push_back(deck.Status()); }
        js["config"] = Config::Get().GetJson();
        js["nmos"] = node.GetStatusJson();
        js["nmos"]["server_up"] = bNmosUp;      //false = node api port is taken
        js["device"] = {
            {"attached", bDevicesUp.load()},
            {"channels", kChannels},
            {"tx_fill_frames", vTxTaps.empty() ? 0 : vTxTaps[0]->FillFrames()},
            {"tx_underruns", vTxTaps.empty() ? 0 : vTxTaps[0]->Underruns()},
            {"tx_trimmed", vTxTaps.empty() ? 0 : vTxTaps[0]->Trimmed()},
            {"rx_fill_frames", vRxTaps.empty() ? 0 : vRxTaps[0]->DelayFrames()},
            {"rx_underruns", vRxTaps.empty() ? 0 : vRxTaps[0]->Underruns()},
            {"clock", bridge.GetStringProperty(kClockSyncSelector)}
        };
        js["monitor"] = monitor.GetStatusJson();
        if(auto* pIs12 = node.GetIs12()) { js["monitors"] = pIs12->GetMonitorJson(); }
        return js;
    };
    auto Action = [&](const std::string& sAction, const json& jsBody) -> json
    {
        int nPort = Config::Get().GetValue<int>("nmos.http_port", 8080);
        int nStatus = 0;

        if(sAction == "list-interfaces")
        {
            //selectable interface list for the setup page
            json jsList = json::array();
            ifaddrs* pList = nullptr;
            if(getifaddrs(&pList) == 0)
            {
                std::set<std::string> setSeen;
                for(auto* p = pList; p; p = p->ifa_next)
                {
                    std::string sName = p->ifa_name ? p->ifa_name : "";
                    if(sName.empty() || !setSeen.insert(sName).second) { continue; }
                    for(const char* sSkip : {"lo", "utun", "awdl", "llw", "gif", "stf", "anpi", "bridge", "ap"})
                    {
                        if(sName.rfind(sSkip, 0) == 0) { sName.clear(); break; }
                    }
                    if(sName.empty()) { continue; }
                    jsList.push_back({{"name", sName}, {"ip", compat::InterfaceIp(sName)}});
                }
                freeifaddrs(pList);
            }
            std::sort(jsList.begin(), jsList.end(),
                      [](const json& a, const json& b) { return a["name"] < b["name"]; });
            return {{"interfaces", jsList}};
        }
        if(sAction == "daemon-restart")
        {
            //reply first, then wind down and exec ourselves (pid survives, so
            //launchd keeps tracking us and the tcc grant stays untouched)
            std::thread([]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                g_bRestart = 1;
                g_bStop = 1;
            }).detach();
            return {{"ok", true}, {"restarting", true}};
        }

        //---- dynamic stream management ----
        if(sAction == "stream-add")
        {
            auto sType = jsBody.value("type", "receiver");
            const char* sKey = sType == "sender" ? "senders" : "receivers";
            auto jsStreams = StreamArray(sKey);
            if(jsStreams.size() >= kMaxStreams) { return {{"error", "stream limit reached"}}; }

            size_t nIndex = jsStreams.size();
            //deterministic slot uid: re-creating a removed stream yields the
            //same nmos uuid again, so controller-side saved routes stay valid
            std::set<std::string> setUids;
            for(const auto& jsStream : jsStreams) { setUids.insert(jsStream.value("uid", "")); }
            std::string sUidPrefix = sType == "sender" ? "tx-" : "rx-";
            std::string sUid;
            for(size_t k = 1; ; k++)
            {
                sUid = sUidPrefix + std::to_string(k);
                if(!setUids.count(sUid)) { break; }
            }
            json jsEntry = {{"uid", sUid},
                            {"channels", std::clamp(jsBody.value("channels", 8), 1, int(kMaxStreamChannels))},
                            {"channel_offset", jsBody.value("channel_offset", 0)}};
            if(jsBody.contains("channel_map")) { jsEntry["channel_map"] = jsBody["channel_map"]; }
            if(sType == "sender")
            {
                jsEntry["enabled"] = jsBody.value("enabled", false);
                jsEntry["source"] = "device";
                jsEntry["label"] = jsBody.value("label", "");
                jsEntry["bits"] = jsBody.value("bits", 24);
                jsEntry["port"] = 5004;
                jsEntry["packet_time_us"] = 1000;
                jsEntry["payload_type"] = 96;
                jsEntry["multicast_primary"] = jsBody.value("multicast",
                    "239.69.145." + std::to_string(10 + nIndex));
                jsEntry["multicast_secondary"] = "";
                jsEntry["multicast_loop"] = true;
                jsEntry["leg1_enabled"] = true;
                jsEntry["leg2_enabled"] = true;
            }
            jsStreams.push_back(jsEntry);
            Config::Get().SetValue(sKey, jsStreams);
            ApplyTopology();
            return {{"ok", true}, {"index", nIndex}};
        }
        if(sAction == "stream-remove")
        {
            auto sType = jsBody.value("type", "receiver");
            const char* sKey = sType == "sender" ? "senders" : "receivers";
            auto jsStreams = StreamArray(sKey);
            auto nIndex = size_t(jsBody.value("index", -1));
            if(nIndex >= jsStreams.size()) { return {{"error", "unknown stream index"}}; }
            jsStreams.erase(nIndex);
            Config::Get().SetValue(sKey, jsStreams);
            ApplyTopology();
            return {{"ok", true}};
        }
        if(sAction == "stream-map")
        {
            auto sType = jsBody.value("type", "receiver");
            const char* sKey = sType == "sender" ? "senders" : "receivers";
            auto jsStreams = StreamArray(sKey);
            auto nIndex = size_t(jsBody.value("index", -1));
            if(nIndex >= jsStreams.size()) { return {{"error", "unknown stream index"}}; }
            if(!jsBody.contains("channel_map") || !jsBody["channel_map"].is_array())
            {
                return {{"error", "channel_map array required"}};
            }
            jsStreams[nIndex]["channel_map"] = jsBody["channel_map"];
            jsStreams[nIndex]["channels"] = int(jsBody["channel_map"].size());
            Config::Get().SetValue(sKey, jsStreams);

            //fast path: only the affected stream is rebuilt - nmos keeps
            //running (resource ids are stable, the channel list re-registers
            //asynchronously via the version bump)
            {
                std::lock_guard<std::mutex> lg(topologyMutex);
                auto vMap = MapFromEntry(jsStreams[nIndex], nIndex);
                if(sType == "sender" && nIndex < vSenders.size())
                {
                    vSenders[nIndex]->Stop();               //stop reader before the tap swap
                    vTxTaps[nIndex] = std::make_shared<TxTap>(vMap);
                    if(kGatewayEngine)
                    {
                        vTxTaps[nIndex]->SetMaxFill(size_t(Config::Get().GetValue<double>("gateway.buffer_ms", 30.0)*48.0) + 448);
                    }
                    txFanout.SetTaps(vTxTaps);
                    ApplySender(nIndex, *vSenders[nIndex], vTxTaps[nIndex].get(), sFilesDir);
                }
                else if(sType == "receiver" && nIndex < vReceivers.size())
                {
                    vReceivers[nIndex]->Stop();             //stop writer before the tap swap
                    vRxTaps[nIndex]->Shutdown();
                    vRxTaps[nIndex] = std::make_shared<RxTap>(vMap);
                    rxMixer.SetTaps(vRxTaps);
                    vReceivers[nIndex]->SetSink(*vRxTaps[nIndex]);      //object stays - nmos threads hold it
                }
            }
            node.BumpVersion();
            ApplyMonitor();     //the monitored receiver's channels may have moved
            return {{"ok", true}};
        }

        //---- stream-scoped ui actions (default: stream 0) ----
        size_t nStream = size_t(std::max(0, jsBody.value("stream", 0)));
        {
            std::lock_guard<std::mutex> lg(topologyMutex);
            if(sAction == "sender-sdp")
            {
                if(nStream >= vSenders.size()) { return {{"error", "unknown sender"}}; }
                auto session = vSenders[nStream]->DescribeSession();
                if(session.vLegs.empty()) { return {{"error", "sender is not running"}}; }
                auto vIps = vSenders[nStream]->GetSourceIps();
                return {{"sdp", rtp::GenerateSdp(session, vIps.empty() ? "0.0.0.0" : vIps[0], vIps)}};
            }
            if(nStream >= vReceivers.size()) { nStream = 0; }
        }
        if(node.ReceiverCount() == 0) { return {{"error", "no receivers configured"}}; }
        auto sBase = "http://127.0.0.1:" + std::to_string(nPort) +
                     "/x-nmos/connection/v1.1/single/receivers/" +
                     node.ReceiverId(std::min(nStream, node.ReceiverCount()-1)) + "/staged";

        if(sAction == "receiver-connect")
        {
            json jsPatch = {{"master_enable", true}, {"activation", {{"mode", "activate_immediate"}}},
                            {"transport_file", {{"type", "application/sdp"}, {"data", jsBody.value("sdp", "")}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
        }
        else if(sAction == "receiver-disconnect")
        {
            json jsPatch = {{"master_enable", false}, {"sender_id", nullptr},
                            {"activation", {{"mode", "activate_immediate"}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
        }
        else if(sAction == "nmos-senders")
        {
            //browse every audio sender the registry knows via the is-04 query api
            auto sRegistry = node.GetStatusJson().value("registry", "");
            if(sRegistry.empty()) { return {{"error", "not registered with a registry"}}; }

            auto ParseVersion = [](const std::string& s) -> std::pair<uint64_t, uint64_t>
            {
                auto nColon = s.find(':');
                if(nColon == std::string::npos) { return {0, 0}; }
                return {strtoull(s.c_str(), nullptr, 10), strtoull(s.c_str()+nColon+1, nullptr, 10)};
            };
            auto FetchAll = [&](const std::string& sUrl) -> std::optional<json>
            {
                json jsAll = json::array();
                std::set<std::string> setIds;
                std::string sQuery = sUrl + "?paging.limit=100";
                for(int nPage = 0; nPage < 20; nPage++)
                {
                    auto jsPage = nmos::HttpJson("GET", sQuery, nullptr, nStatus);
                    if(!jsPage || !jsPage->is_array()) { return jsAll.empty() ? std::nullopt : std::optional(jsAll); }
                    std::string sOldest;
                    size_t nNew = 0;
                    for(const auto& js : *jsPage)
                    {
                        auto sVersion = js.value("version", "");
                        if(sOldest.empty() || ParseVersion(sVersion) < ParseVersion(sOldest)) { sOldest = sVersion; }
                        if(setIds.insert(js.value("id", "")).second) { jsAll.push_back(js); nNew++; }
                    }
                    if(jsPage->size() < 100 || nNew == 0 || sOldest.empty()) { break; }
                    sQuery = sUrl + "?paging.limit=100&paging.until=" + sOldest;
                }
                return jsAll;
            };

            auto jsSenders = FetchAll(sRegistry + "/x-nmos/query/v1.3/senders");
            if(!jsSenders)
            {
                return {{"error", "query api not reachable (status " + std::to_string(nStatus) + ")"}};
            }
            auto jsFlows = FetchAll(sRegistry + "/x-nmos/query/v1.3/flows");
            auto jsDevices = FetchAll(sRegistry + "/x-nmos/query/v1.3/devices");

            auto Find = [](const std::optional<json>& jsList, const std::string& sId) -> json
            {
                if(!jsList || !jsList->is_array() || sId.empty()) { return json::object(); }
                for(const auto& js : *jsList)
                {
                    if(js.value("id", "") == sId) { return js; }
                }
                return json::object();
            };
            auto StringOr = [](const json& js, const char* sKey) -> std::string
            {
                return js.contains(sKey) && js[sKey].is_string() ? js[sKey].get<std::string>() : "";
            };

            json jsList = json::array();
            for(const auto& jsSender : *jsSenders)
            {
                auto jsFlow = Find(jsFlows, StringOr(jsSender, "flow_id"));
                auto sFormat = jsFlow.value("format", "");
                if(!sFormat.empty() && sFormat != "urn:x-nmos:format:audio") { continue; }
                bool bActive = true;
                if(jsSender.contains("subscription") && jsSender["subscription"].is_object())
                {
                    bActive = jsSender["subscription"].value("active", true);
                }
                jsList.push_back({
                    {"id", jsSender.value("id", "")},
                    {"label", jsSender.value("label", "")},
                    {"device", Find(jsDevices, StringOr(jsSender, "device_id")).value("label", "")},
                    {"media_type", jsFlow.value("media_type", "")},
                    {"manifest_href", StringOr(jsSender, "manifest_href")},
                    {"active", bActive},
                    {"is_self", node.SenderIndex(jsSender.value("id", "")) != std::string::npos}
                });
            }
            return {{"senders", jsList}};
        }
        else if(sAction == "nmos-connect")
        {
            auto sSenderId = jsBody.value("sender_id", "");
            auto sManifest = jsBody.value("manifest_href", "");
            if(sManifest.rfind("http://", 0) != 0) { return {{"error", "sender has no usable manifest url"}}; }

            auto sSdp = nmos::HttpText("GET", sManifest, nullptr, nStatus);
            if(!sSdp || nStatus != 200 || sSdp->find("m=audio") == std::string::npos)
            {
                return {{"error", "could not fetch sdp from sender (status " + std::to_string(nStatus) + ")"}};
            }
            json jsPatch = {{"master_enable", true}, {"sender_id", sSenderId},
                            {"activation", {{"mode", "activate_immediate"}}},
                            {"transport_file", {{"type", "application/sdp"}, {"data", *sSdp}}}};
            nmos::HttpJson("PATCH", sBase, &jsPatch, nStatus);
            return {{"status", nStatus}};
        }
        else if(sAction.rfind("player-", 0) == 0)
        {
            auto nPlayer = jsBody.value("player", 0);
            if(nPlayer < 0 || size_t(nPlayer) >= playerDecks.size()) { return {{"error", "no such player"}}; }
            auto& deck = playerDecks[size_t(nPlayer)];
            if(sAction == "player-load")
            {
                auto sFile = jsBody.value("file", "");
                if(sFile.empty() || sFile.find('/') != std::string::npos ||
                   sFile.find("..") != std::string::npos)
                {
                    return {{"error", "bad file name"}};
                }
                if(!deck.Load(sFilesDir + "/" + sFile, sFile))
                {
                    return {{"error", "cannot load " + sFile}};
                }
            }
            else if(sAction == "player-ctrl") { deck.Control(jsBody.value("command", "")); }
            else if(sAction == "player-seek") { deck.Seek(jsBody.value("seconds", 0.0)); }
            else if(sAction == "player-set")  { deck.Settings(jsBody); }
            else { return {{"error", "unknown player action"}}; }
            PersistPlayers();
            return {{"ok", true}};
        }
        else if(sAction == "network-status")
        {
            //static readout - network configuration stays with macos system settings
            json jsIfs = json::array();
            auto vInterfaces = Interfaces();
            const char* ROLES[] = {"amber", "blue"};
            for(size_t nRole = 0; nRole < vInterfaces.size() && nRole < 2; nRole++)
            {
                auto sIp = compat::InterfaceIp(vInterfaces[nRole]);
                jsIfs.push_back({{"role", ROLES[nRole]}, {"interface", vInterfaces[nRole]},
                                 {"link", !sIp.empty()}, {"connection", ""}, {"method", "system"},
                                 {"address", sIp}, {"gateway", ""}});
            }
            return {{"interfaces", jsIfs}};
        }
        else if(sAction == "network-apply")
        {
            return {{"error", "network configuration is managed in macOS system settings"}};
        }
        return {{"status", nStatus}};
    };

    auto MetersJson = [&]() -> json
    {
        std::lock_guard<std::mutex> lg(topologyMutex);
        json jsRx = {-120.0, -120.0}, jsTx = {-120.0, -120.0};
        json jsRxAll = json::array(), jsTxAll = json::array();
        for(auto& pReceiver : vReceivers)
        {
            jsRxAll.push_back({pReceiver->GetMeters().LeftDb(), pReceiver->GetMeters().RightDb()});
        }
        for(auto& pSender : vSenders)
        {
            jsTxAll.push_back({pSender->GetMeters().LeftDb(), pSender->GetMeters().RightDb()});
        }
        if(!jsRxAll.empty()) { jsRx = jsRxAll[0]; }
        if(!jsTxAll.empty()) { jsTx = jsTxAll[0]; }
        return {{"meters", {{"rx", jsRx}, {"tx", jsTx}, {"rx_all", jsRxAll}, {"tx_all", jsTxAll},
                            {"in64", inMeters.Db()}, {"out64", outMeters.Db()}}}};
    };
    web.Start(Config::Get().GetValue<int>("web.port", 8180), sWebRoot, sFilesDir, StatusJson, MetersJson,
              [&](const json& jsChanged)
              {
                  //leg toggles apply live - other sender keys restart that sender
                  auto SenderKey = [](const std::string& sKey, std::string& sSub) -> int
                  {
                      if(sKey.rfind("senders.", 0) != 0) { return -1; }
                      auto nDot = sKey.find('.', 8);
                      if(nDot == std::string::npos) { return -1; }
                      sSub = sKey.substr(nDot+1);
                      return atoi(sKey.c_str()+8);
                  };
                  std::lock_guard<std::mutex> lg(topologyMutex);
                  std::set<size_t> setRestart;
                  bool bMonitorChanged = false;
                  bool bBump = false;
                  for(const auto& [sKey, jsValue] : jsChanged.items())
                  {
                      std::string sSub;
                      int n = SenderKey(sKey, sSub);
                      if(n >= 0 && size_t(n) < vSenders.size())
                      {
                          if(sSub == "leg1_enabled" || sSub == "leg2_enabled")
                          {
                              auto jsStreams = StreamArray("senders");
                              vSenders[size_t(n)]->SetLegEnabled(0, jsStreams[size_t(n)].value("leg1_enabled", true));
                              vSenders[size_t(n)]->SetLegEnabled(1, jsStreams[size_t(n)].value("leg2_enabled", true));
                              bBump = true;
                          }
                          else
                          {
                              setRestart.insert(size_t(n));
                          }
                      }
                      if(sKey.rfind("monitor.", 0) == 0) { bMonitorChanged = true; }
                      if(sKey == "ptp.domain") { ptpClient.SetDomain(jsValue.get<int>()); }
                      if(sKey == "ptp.domain_auto") { ptpClient.SetAutoDomain(jsValue.get<bool>()); }
                      if(sKey == "receiver.monitor_left" || sKey == "receiver.monitor_right")
                      {
                          if(!vReceivers.empty())
                          {
                              vReceivers[0]->SetMonitor(Config::Get().GetValue<int>("receiver.monitor_left", -1),
                                                        Config::Get().GetValue<int>("receiver.monitor_right", -1));
                          }
                      }
                  }
                  bool bNetworkChanged = false;
                  for(const auto& [sKey, jsValue] : jsChanged.items())
                  {
                      if(sKey.rfind("network.", 0) == 0) { bNetworkChanged = true; }
                  }
                  if(bNetworkChanged)
                  {
                      //new interface set: restart ptp (amber+blue monitoring) and
                      //every running sender; receivers re-join on the next patch
                      ptpClient.Run(Interfaces(), Config::Get().GetValue<int>("ptp.domain", 0));
                      for(size_t n = 0; n < vSenders.size(); n++) { setRestart.insert(n); }
                  }
                  for(size_t n : setRestart)
                  {
                      ApplySender(n, *vSenders[n], vTxTaps[n].get(), sFilesDir);
                      bBump = true;
                  }
                  if(bBump) { node.BumpVersion(); }
                  if(bMonitorChanged) { ApplyMonitor(); }
              }, Action);

    //---- 1 Hz housekeeping: clock discipline, bcp-008 status feed ----
    auto HousekeepingLoop = [&]()
    {
    std::vector<std::array<uint64_t, 2>> vLastLost;
    std::vector<uint64_t> vLastLate, vLastPlayed, vLastSent, vLastErrors;
    while(!g_bStop)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        //reattach after a coreaudiod restart, retry while the driver is absent
        if(bReopenPending.exchange(false))
        {
            LOG_WARN("main") << "coreaudiod restarted - reattaching devices";
            bridge.Close();
            mainDevice.Close();
            OpenDevices();
        }
        else if(!bDevicesUp)
        {
            bridge.Close();
            mainDevice.Close();
            OpenDevices();
        }

        //re-announce when our addresses change (dhcp, dock swap, new site) -
        //otherwise the registry keeps hrefs that no longer exist
        {
            static std::vector<std::string> vLastIps;
            static int nIpTick = 0;
            if(++nIpTick % 5 == 0)
            {
                auto vIps = node.GetHostIps();
                if(vIps != vLastIps)
                {
                    if(!vLastIps.empty())
                    {
                        LOG_INFO("main") << "host addresses changed - re-announcing nmos resources";
                        node.BumpVersion();
                    }
                    vLastIps = vIps;
                }
            }
        }

        if(!bNmosUp && Config::Get().GetValue<bool>("nmos.enabled", true))
        {
            bNmosUp = node.Start(Config::Get().GetValue<int>("nmos.http_port", 8080), Interfaces(), callbacks);
            if(bNmosUp) { LOG_INFO("main") << "nmos server started (port was busy at launch)"; }
        }

        //retry senders that are enabled but not running (e.g. their interface
        //was unplugged at start and came back)
        static int nRetryTick = 0;
        if(++nRetryTick % 10 == 0)
        {
            std::lock_guard<std::mutex> lg(topologyMutex);
            auto jsSenders = StreamArray("senders");
            for(size_t n = 0; n < vSenders.size() && n < jsSenders.size(); n++)
            {
                if(jsSenders[n].value("enabled", false) && !vSenders[n]->IsRunning())
                {
                    ApplySender(n, *vSenders[n], vTxTaps[n].get(), sFilesDir);
                }
            }
        }

        //discipline the driver clock with the current ptp mapping
        if(bDevicesUp)
        {
            auto mapping = ptpClient.GetMapping();
            auto nRatePpb = static_cast<long long>((mapping.dRate - 1.0) * 1e9);
            std::ostringstream ss;
            ss << nRatePpb << " " << (mapping.bLocked ? 1 : 0);
            bridge.SetStringProperty(kClockSyncSelector, ss.str());
        }

        std::lock_guard<std::mutex> lg(topologyMutex);
        vLastLost.resize(vReceivers.size(), {0, 0});
        vLastLate.resize(vReceivers.size(), 0);
        vLastPlayed.resize(vReceivers.size(), 0);
        vLastSent.resize(vSenders.size(), 0);
        vLastErrors.resize(vSenders.size(), 0);

        auto* pIs12 = node.GetIs12();
        if(!pIs12) { continue; }

        //sync domain (all monitors)
        bool bSynced = ptpClient.IsSynced();
        auto sGm = ptpClient.GrandmasterId();
        pIs12->SetSyncSource(sGm.empty() ? std::nullopt : std::optional(sGm));
        int nSync = sGm.empty() ? 0 : (bSynced ? 1 : 3);
        for(size_t n = 0; n < vReceivers.size(); n++)
        {
            pIs12->UpdateReceiverDomain(n, nmos::Is12Server::SYNC, nSync, bSynced || sGm.empty() ? "" : "not locked to ptp");
        }
        for(size_t n = 0; n < vSenders.size(); n++)
        {
            pIs12->UpdateSenderDomain(n, nmos::Is12Server::SYNC, nSync, bSynced || sGm.empty() ? "" : "not locked to ptp");
        }

        //receiver path + stream per stream
        for(size_t n = 0; n < vReceivers.size(); n++)
        {
            auto& receiver = *vReceivers[n];
            auto jsRx = receiver.GetStatusJson();
            if(!receiver.IsRunning())
            {
                pIs12->UpdateReceiverDomain(n, nmos::Is12Server::PATH, 0, "");
                pIs12->UpdateReceiverDomain(n, nmos::Is12Server::STREAM, 0, "");
                continue;
            }
            if(!receiver.IsReceiving())
            {
                pIs12->UpdateReceiverDomain(n, nmos::Is12Server::PATH, 3, "no rtp packets arriving");
                continue;
            }
            auto nPlayed = jsRx.value("played", 0ULL);
            if(nPlayed < vLastPlayed[n]) { vLastPlayed[n] = 0; }    //receiver was reconfigured
            pIs12->UpdateReceiverDomain(n, nmos::Is12Server::PATH, 1, "");
            pIs12->UpdateReceiverDomain(n, nmos::Is12Server::STREAM,
                nPlayed > vLastPlayed[n] ? 1 : 2, nPlayed > vLastPlayed[n] ? "" : "no audio decoded");
            vLastPlayed[n] = nPlayed;

            bool bPrimary = false, bSecondary = true;
            const auto& jsLegs = jsRx["legs"];
            for(size_t nLeg = 0; nLeg < jsLegs.size() && nLeg < 2; nLeg++)
            {
                auto nLost = receiver.LostPackets(nLeg);
                if(nLost < vLastLost[n][nLeg]) { vLastLost[n][nLeg] = 0; }
                if(nLost > vLastLost[n][nLeg])
                {
                    pIs12->AddReceiverLost(n, jsLegs[nLeg].value("interface", "leg"+std::to_string(nLeg)),
                                           nLost - vLastLost[n][nLeg]);
                    vLastLost[n][nLeg] = nLost;
                }
                if(nLeg == 0) { bPrimary = jsLegs[nLeg].value("active", false); }
                else          { bSecondary = jsLegs[nLeg].value("active", false); }
            }
            int nLink = (bPrimary && bSecondary) ? 1 : ((bPrimary || bSecondary) ? 2 : 3);
            pIs12->UpdateReceiverDomain(n, nmos::Is12Server::LINK, nLink,
                nLink == 1 ? "" : (nLink == 2 ? "one leg down" : "all legs down"));

            auto nLate = receiver.LatePackets();
            if(nLate < vLastLate[n]) { vLastLate[n] = 0; }
            if(nLate > vLastLate[n])
            {
                pIs12->AddReceiverLate(n, nLate - vLastLate[n]);
                vLastLate[n] = nLate;
            }
        }

        //sender path + essence per stream
        for(size_t n = 0; n < vSenders.size(); n++)
        {
            auto& sender = *vSenders[n];
            auto jsTx = sender.GetStatusJson();
            if(!sender.IsRunning())
            {
                pIs12->UpdateSenderDomain(n, nmos::Is12Server::PATH, 0, "");
                pIs12->UpdateSenderDomain(n, nmos::Is12Server::STREAM, 0, "");
                continue;
            }
            if(jsTx.value("waiting_for_ptp", false))
            {
                pIs12->UpdateSenderDomain(n, nmos::Is12Server::PATH, 3, "waiting for ptp lock");
                continue;
            }
            auto nSent = jsTx.value("packets_sent", 0ULL);
            if(nSent < vLastSent[n]) { vLastSent[n] = 0; }          //sender was reconfigured
            pIs12->UpdateSenderDomain(n, nmos::Is12Server::PATH, nSent > vLastSent[n] ? 1 : 3,
                                      nSent > vLastSent[n] ? "" : "transmission stalled");
            vLastSent[n] = nSent;
            pIs12->UpdateSenderDomain(n, nmos::Is12Server::STREAM, sender.IsSendingAudio() ? 1 : 2,
                                      sender.IsSendingAudio() ? "" : "source exhausted");
            auto nErrors = jsTx.value("send_errors", 0ULL);
            if(nErrors < vLastErrors[n]) { vLastErrors[n] = 0; }
            if(nErrors > vLastErrors[n])
            {
                pIs12->AddSenderErrors(n, "socket_errors", nErrors - vLastErrors[n]);
                vLastErrors[n] = nErrors;
            }
        }
    }

    };

    //menu bar app: engine threads keep running, the main thread hosts the
    //status item; quitting from the menu (or SIGTERM) winds everything down
    auto MenuStatus = [&]() -> std::string
    {
        size_t nRxActive = 0, nTxActive = 0, nRx = 0, nTx = 0;
        {
            std::lock_guard<std::mutex> lg(topologyMutex);
            nRx = vReceivers.size();
            nTx = vSenders.size();
            for(auto& pReceiver : vReceivers) { if(pReceiver->IsReceiving()) { nRxActive++; } }
            for(auto& pSender : vSenders) { if(pSender->IsRunning()) { nTxActive++; } }
        }
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "PTP %s · TX %zu/%zu · RX %zu/%zu",
                 ptpClient.IsSynced() ? "locked" : "–", nTxActive, nTx, nRxActive, nRx);
        return buffer;
    };
    std::thread housekeeping([&]() { HousekeepingLoop(); StopMenuBar(); });
    RunMenuBar([]() { g_bStop = 1; }, MenuStatus);
    housekeeping.join();

    LOG_INFO("main") << "shutting down";
    if(captureThread.joinable()) { captureThread.join(); }
    web.Stop();
    node.Stop();
    for(auto& pSender : vSenders) { pSender->Stop(); }
    for(auto& pTap : vRxTaps) { pTap->Shutdown(); }     //unblocks stuck playout writes
    for(auto& pReceiver : vReceivers) { pReceiver->Stop(); }
    monitor.Stop();
    bridge.Close();
    mainDevice.Close();
    ptpClient.Stop();

    if(g_bRestart && g_argv)
    {
        LOG_INFO("main") << "restarting";
        execv(g_argv[0], g_argv);
    }
    return 0;
}
