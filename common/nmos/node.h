#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"
#include "rtp/sdp.h"

struct mg_context;

namespace pan::rtp { class RtpSender; class RtpReceiver; }
namespace pan::ptp { class PtpClient; }

namespace pan::nmos
{
    class ConnectionApi;
    class Is12Server;

    /** http request helpers (civetweb client) - registration client, query api **/
    std::optional<std::string> HttpText(const std::string& sMethod, const std::string& sUrl,
                                        const std::string* pBody, int& nStatus);
    std::optional<nlohmann::json> HttpJson(const std::string& sMethod, const std::string& sUrl,
                                           const nlohmann::json* pBody, int& nStatus);

    std::string MakeUuid(const std::string& sSeed);

    /** IS-04 node with N senders + N receivers (one source/flow per sender):
    *   resources, node api, registration + heartbeat. Owns the nmos http
    *   server (default port 8080) that also carries IS-05 and IS-12.
    **/
    class NmosNode
    {
        public:
            struct Callbacks
            {
                //receiver index: enable with parsed sdp, or disable
                std::function<void(size_t, bool, const std::optional<rtp::SdpSession>&)> onReceiver;
                //sender index: master_enable
                std::function<void(size_t, bool)> onSender;
            };

            NmosNode(ptp::PtpClient& ptpClient,
                     std::vector<std::unique_ptr<rtp::RtpSender>>& vSenders,
                     std::vector<std::unique_ptr<rtp::RtpReceiver>>& vReceivers);
            ~NmosNode();

            bool Start(int nPort, const std::vector<std::string>& vInterfaces, Callbacks callbacks);
            void Stop();

            const std::string& NodeId() const     { return m_sNodeId; }
            const std::string& DeviceId() const   { return m_sDeviceId; }

            size_t SenderCount() const   { return m_vSenderIds.size(); }
            size_t ReceiverCount() const { return m_vReceiverIds.size(); }
            const std::string& SenderId(size_t n) const   { return m_vSenderIds[n]; }
            const std::string& ReceiverId(size_t n) const { return m_vReceiverIds[n]; }
            const std::string& SourceId(size_t n) const   { return m_vSourceIds[n]; }
            const std::string& FlowId(size_t n) const     { return m_vFlowIds[n]; }
            //index for a uuid, npos when unknown
            size_t SenderIndex(const std::string& sId) const;
            size_t ReceiverIndex(const std::string& sId) const;
            //stream 0 shortcuts (web ui operates on the first stream)
            const std::string& SenderId() const   { return m_vSenderIds[0]; }
            const std::string& ReceiverId() const { return m_vReceiverIds[0]; }

            nlohmann::json GetStatusJson() const;
            Is12Server* GetIs12() { return m_pIs12.get(); }
            ConnectionApi* GetConnection() { return m_pConnection.get(); }
            void BumpVersion();     //resource change -> re-register

            //resource builders (also served by the node api)
            nlohmann::json BuildSelf() const;
            nlohmann::json BuildDevice() const;
            nlohmann::json BuildSource(size_t n) const;
            nlohmann::json BuildFlow(size_t n) const;
            nlohmann::json BuildSender(size_t n) const;
            nlohmann::json BuildReceiver(size_t n) const;

            std::vector<std::string> GetHostIps() const;
            int Port() const { return m_nPort; }

            ptp::PtpClient& Ptp() { return m_ptp; }
            rtp::RtpSender& Sender(size_t n = 0) { return *m_vSenders[n]; }
            rtp::RtpReceiver& Receiver(size_t n = 0) { return *m_vReceivers[n]; }

            //per-sender config object ("senders" array entry, empty object when absent)
            static nlohmann::json SenderConfig(size_t n);

        private:
            void StopLocked();
            void RegistrationLoop();
            bool RegisterAll(const std::string& sRegistryUrl);
            void RegisterNodeApi();
            void RegisterAnnotationApi();
            std::string Version() const;

            ptp::PtpClient& m_ptp;
            std::vector<std::unique_ptr<rtp::RtpSender>>& m_vSenders;
            std::vector<std::unique_ptr<rtp::RtpReceiver>>& m_vReceivers;

            mg_context* m_pServer = nullptr;
            std::unique_ptr<ConnectionApi> m_pConnection;
            std::unique_ptr<Is12Server> m_pIs12;

            int m_nPort = 8080;
            std::vector<std::string> m_vInterfaces;
            std::string m_sHostname;
            std::string m_sNodeId, m_sDeviceId;
            std::vector<std::string> m_vSourceIds, m_vFlowIds, m_vSenderIds, m_vReceiverIds;

            std::thread m_regThread;
            std::mutex m_lifecycleMutex;    //Start/Stop may race (web action vs retry)
            std::atomic<bool> m_bRun{false};
            std::atomic<uint64_t> m_nVersion{0};
            std::atomic<uint64_t> m_nVersionTime{uint64_t(time(nullptr))};
            std::atomic<uint64_t> m_nRegisteredVersion{0};
            mutable std::mutex m_mutex;
            std::string m_sRegistry;        //current registry url or empty
            std::string m_sRegistryStatus;  //for the ui
    };
}
