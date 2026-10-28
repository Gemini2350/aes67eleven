#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include "json.hpp"

struct mg_context;
struct mg_connection;

namespace pan::nmos
{
    class NmosNode;

    //status enums (BCP-008): 0 inactive, 1 healthy, 2 partially healthy, 3 unhealthy
    //link: 1 all up, 2 some down, 3 all down / sync: 0 not used
    struct McDomain
    {
        int nStatus = 0;
        std::string sMessage;
        uint32_t nTransitions = 0;
        //bcp-008 statusReportingDelay: a less healthy state is held back here
        //until it has persisted for the delay
        int nPendingStatus = -1;
        std::chrono::steady_clock::time_point pendingSince;
    };

    /** IS-12 control protocol websocket at /x-nmos/ncp/v1.0 with the minimal
    *   MS-05-02 model: root block, device manager, class manager and one
    *   NcReceiverMonitor (1.2.2.1) / NcSenderMonitor (1.2.2.2) per stream.
    *   Subscriptions and command responses are per websocket connection.
    **/
    class Is12Server
    {
        public:
            explicit Is12Server(NmosNode& node);
            void Register(mg_context* pServer);

            //domain indices for UpdateDomain
            enum { LINK=0, PATH=1, SYNC=2, STREAM=3 };

            void UpdateReceiverDomain(size_t nStream, int nDomain, int nStatus, const std::string& sMessage);
            void UpdateSenderDomain(size_t nStream, int nDomain, int nStatus, const std::string& sMessage);
            void SetSyncSource(const std::optional<std::string>& sSourceId);
            void AddReceiverLost(size_t nStream, const std::string& sLeg, uint64_t nPackets);
            void AddReceiverLate(size_t nStream, uint64_t nPackets);
            void AddSenderErrors(size_t nStream, const std::string& sCounter, uint64_t nErrors);

            nlohmann::json GetMonitorJson() const;      //for the web ui

        private:
            struct Monitor
            {
                int nOid;
                std::string sRole;
                std::string sLabel;
                std::string sTouchpointType;
                std::string sTouchpointId;
                McDomain aDomains[4];
                int nOverall = 0;
                std::string sOverallMessage;
                std::optional<std::string> sSyncSource;
                std::map<std::string, uint64_t> mCounters1;     //lost / transmission errors
                std::map<std::string, uint64_t> mCounters2;     //late (receiver only)
                bool bIsReceiver;
            };

            static int WsConnect(const mg_connection* pConn, void* pUser);
            static void WsReady(mg_connection* pConn, void* pUser);
            static int WsData(mg_connection* pConn, int nBits, char* pData, size_t nSize, void* pUser);
            static void WsClose(const mg_connection* pConn, void* pUser);

            void HandleMessage(mg_connection* pConn, const nlohmann::json& jsMessage);
            nlohmann::json InvokeMethod(Monitor* pMonitor, int nOid, int nLevel, int nIndex, const nlohmann::json& jsArgs);
            nlohmann::json GetProperty(int nOid, int nLevel, int nIndex);
            nlohmann::json MemberDescriptor(int nOid, const std::string& sRole, const std::string& sLabel,
                                            const std::vector<int>& vClassId) const;
            void SendTo(mg_connection* pConn, const nlohmann::json& jsMessage);
            void UpdateDomain(Monitor& monitor, int nDomain, int nStatus, const std::string& sMessage);
            void RecomputeOverall(Monitor& monitor);
            void Notify(int nOid, int nLevel, int nIndex, const nlohmann::json& jsValue);
            Monitor* FindMonitor(int nOid);

            Monitor* RxMonitor(size_t nStream);
            Monitor* TxMonitor(size_t nStream);

            NmosNode& m_node;
            mutable std::mutex m_mutex;
            //receivers first, then senders (oids from 10 upwards)
            std::vector<Monitor> m_vMonitors;
            size_t m_nReceivers = 0;
            std::map<mg_connection*, std::set<int>> m_mSubscriptions;
    };
}
