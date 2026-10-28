#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include "json.hpp"

struct mg_context;
struct mg_connection;

namespace pan::web
{
    /** ui web server: static spa + rest api + websocket status push **/
    class WebServer
    {
        public:
            using StatusProvider = std::function<nlohmann::json()>;
            using ConfigApplied = std::function<void(const nlohmann::json&)>;   //changed keys
            using ActionHandler = std::function<nlohmann::json(const std::string&, const nlohmann::json&)>;

            ~WebServer();

            /** pMeters delivers a tiny high-rate message (20 Hz) for the level
            *   meters, pStatus the full state (4 Hz) **/
            bool Start(int nPort, const std::string& sWebRoot, const std::string& sFilesDir,
                       StatusProvider pStatus, StatusProvider pMeters, ConfigApplied pApplied, ActionHandler pAction);
            void Stop();

        private:
            int HandleApi(mg_connection* pConn);
            void PushLoop();

            static int WsConnect(const mg_connection*, void*) ;
            static void WsReady(mg_connection* pConn, void* pUser);
            static void WsClose(const mg_connection* pConn, void* pUser);

            mg_context* m_pServer = nullptr;
            std::string m_sFilesDir;
            StatusProvider m_pStatus;
            StatusProvider m_pMeters;
            ConfigApplied m_pApplied;
            ActionHandler m_pAction;

            std::thread m_pushThread;
            std::atomic<bool> m_bRun{false};
            std::mutex m_mutex;
            std::set<mg_connection*> m_setClients;
    };
}
