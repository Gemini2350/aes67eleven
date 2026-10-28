#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "json.hpp"
#include "rtp/sdp.h"

struct mg_context;
struct mg_connection;

namespace pan::nmos
{
    class NmosNode;

    /** IS-05 v1.1 connection api: /single with N senders + N receivers,
    *   immediate activation and up to two transport param legs (ST 2022-7).
    **/
    class ConnectionApi
    {
        public:
            using ReceiverCallback = std::function<void(size_t, bool, const std::optional<rtp::SdpSession>&)>;
            using SenderCallback = std::function<void(size_t, bool)>;

            ConnectionApi(NmosNode& node, ReceiverCallback onReceiver, SenderCallback onSender);
            void Register(mg_context* pServer);

            nlohmann::json ReceiverSenderId(size_t n) const;    //active subscription for IS-04

        private:
            int Handle(mg_connection* pConn);
            void PatchSender(size_t n, const nlohmann::json& jsPatch);
            void PatchReceiver(size_t n, const nlohmann::json& jsPatch);
            void ActivateSender(size_t n);
            void ActivateReceiver(size_t n);
            nlohmann::json SenderActiveNow(size_t n) const;
            nlohmann::json BuildConstraints(size_t nLegs) const;
            std::string SenderConfigKey(size_t n, const char* sKey) const;

            NmosNode& m_node;
            ReceiverCallback m_onReceiver;
            SenderCallback m_onSender;

            mutable std::mutex m_mutex;
            std::vector<nlohmann::json> m_vSenderStaged, m_vSenderActive;
            std::vector<nlohmann::json> m_vReceiverStaged, m_vReceiverActive;
    };
}
