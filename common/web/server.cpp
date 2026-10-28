#include "server.h"
#include <dirent.h>
#include <fstream>
#include "civetweb.h"
#include "config.h"
#include "log.h"

using namespace pan::web;
using json = nlohmann::json;

namespace
{
    void SendJson(mg_connection* pConn, int nStatus, const json& js)
    {
        auto sBody = js.dump();
        mg_printf(pConn, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  nStatus, nStatus < 300 ? "OK" : "Error", sBody.size());
        mg_write(pConn, sBody.data(), sBody.size());
    }

    std::string ReadBody(mg_connection* pConn)
    {
        std::string sBody;
        char buffer[8192];
        int nRead;
        while((nRead = mg_read(pConn, buffer, sizeof(buffer))) > 0)
        {
            sBody.append(buffer, static_cast<size_t>(nRead));
        }
        return sBody;
    }
}

WebServer::~WebServer()
{
    Stop();
}

bool WebServer::Start(int nPort, const std::string& sWebRoot, const std::string& sFilesDir,
                      StatusProvider pStatus, StatusProvider pMeters, ConfigApplied pApplied, ActionHandler pAction)
{
    Stop();
    m_sFilesDir = sFilesDir;
    m_pStatus = std::move(pStatus);
    m_pMeters = std::move(pMeters);
    m_pApplied = std::move(pApplied);
    m_pAction = std::move(pAction);

    std::string sPort = std::to_string(nPort);
    const char* options[] = {
        "listening_ports", sPort.c_str(),
        "document_root", sWebRoot.c_str(),
        "num_threads", "6",
        "enable_directory_listing", "no",
        nullptr
    };
    m_pServer = mg_start(nullptr, nullptr, options);
    if(!m_pServer)
    {
        LOG_ERROR("web") << "cannot listen on port " << nPort;
        return false;
    }

    mg_set_request_handler(m_pServer, "/api", [](mg_connection* pConn, void* pUser) -> int
    {
        return static_cast<WebServer*>(pUser)->HandleApi(pConn);
    }, this);
    mg_set_websocket_handler(m_pServer, "/ws", WsConnect, WsReady, nullptr, WsClose, this);

    //the nmos apis live on their own port (nmos.http_port) - redirect anyone
    //who tries them on the ui port instead of serving a confusing 404
    mg_set_request_handler(m_pServer, "/x-nmos", [](mg_connection* pConn, void*) -> int
    {
        const auto* pInfo = mg_get_request_info(pConn);
        int nNmosPort = Config::Get().GetValue<int>("nmos.http_port", 8080);
        std::string sHost = "localhost";
        if(const char* sHeader = mg_get_header(pConn, "Host"))
        {
            sHost = sHeader;
            if(auto nColon = sHost.find(':'); nColon != std::string::npos) { sHost.resize(nColon); }
        }
        std::string sTarget = "http://" + sHost + ":" + std::to_string(nNmosPort) + pInfo->local_uri;
        if(pInfo->query_string && pInfo->query_string[0]) { sTarget += std::string("?") + pInfo->query_string; }
        mg_printf(pConn, "HTTP/1.1 307 Temporary Redirect\r\nLocation: %s\r\n"
                         "Content-Length: 0\r\nConnection: close\r\n\r\n", sTarget.c_str());
        return 307;
    }, this);

    m_bRun = true;
    m_pushThread = std::thread(&WebServer::PushLoop, this);
    LOG_INFO("web") << "ui on port " << nPort << " root " << sWebRoot;
    return true;
}

void WebServer::Stop()
{
    m_bRun = false;
    if(m_pushThread.joinable()) { m_pushThread.join(); }
    if(m_pServer) { mg_stop(m_pServer); m_pServer = nullptr; }
}

int WebServer::WsConnect(const mg_connection*, void*) { return 0; }

void WebServer::WsReady(mg_connection* pConn, void* pUser)
{
    auto* pThis = static_cast<WebServer*>(pUser);
    std::lock_guard<std::mutex> lg(pThis->m_mutex);
    pThis->m_setClients.insert(pConn);
}

void WebServer::WsClose(const mg_connection* pConn, void* pUser)
{
    auto* pThis = static_cast<WebServer*>(pUser);
    std::lock_guard<std::mutex> lg(pThis->m_mutex);
    pThis->m_setClients.erase(const_cast<mg_connection*>(pConn));
}

void WebServer::PushLoop()
{
    //50 ms ticks: meters go out on every tick (20 Hz), the full status on
    //every fifth (4 Hz) - the meter message is ~60 bytes, so the fast path
    //costs next to nothing
    int nTick = 0;
    while(m_bRun)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        nTick++;

        std::set<mg_connection*> setClients;
        {
            std::lock_guard<std::mutex> lg(m_mutex);
            setClients = m_setClients;
        }
        if(setClients.empty()) { continue; }

        bool bFull = (nTick % 5 == 0);
        if(bFull ? !m_pStatus : !m_pMeters) { continue; }
        auto sMessage = (bFull ? m_pStatus() : m_pMeters()).dump();
        for(auto* pConn : setClients)
        {
            mg_lock_connection(pConn);
            mg_websocket_write(pConn, MG_WEBSOCKET_OPCODE_TEXT, sMessage.data(), sMessage.size());
            mg_unlock_connection(pConn);
        }
    }
}

int WebServer::HandleApi(mg_connection* pConn)
{
    const auto* pInfo = mg_get_request_info(pConn);
    std::string sUri(pInfo->local_uri);
    std::string sMethod(pInfo->request_method);

    if(sUri == "/api/status")
    {
        SendJson(pConn, 200, m_pStatus ? m_pStatus() : json::object());
    }
    else if(sUri == "/api/config" && sMethod == "GET")
    {
        SendJson(pConn, 200, Config::Get().GetJson());
    }
    else if(sUri == "/api/config" && sMethod == "POST")
    {
        try
        {
            auto jsUpdates = json::parse(ReadBody(pConn));
            Config::Get().Update(jsUpdates);
            if(m_pApplied) { m_pApplied(jsUpdates); }
            SendJson(pConn, 200, {{"ok", true}});
        }
        catch(const std::exception& e)
        {
            SendJson(pConn, 400, {{"error", e.what()}});
        }
    }
    else if(sUri == "/api/files" && sMethod == "GET")
    {
        json jsFiles = json::array();
        if(DIR* pDir = opendir(m_sFilesDir.c_str()))
        {
            while(dirent* pEntry = readdir(pDir))
            {
                std::string sName(pEntry->d_name);
                if(sName.size() > 4 && (sName.substr(sName.size()-4) == ".wav" || sName.substr(sName.size()-4) == ".mp3"))
                {
                    jsFiles.push_back(sName);
                }
            }
            closedir(pDir);
        }
        SendJson(pConn, 200, jsFiles);
    }
    else if(sUri.rfind("/api/files/", 0) == 0 && sMethod == "PUT")
    {
        auto sName = sUri.substr(11);
        if(sName.find('/') != std::string::npos || sName.find("..") != std::string::npos)
        {
            SendJson(pConn, 400, {{"error", "bad name"}});
            return 200;
        }
        std::ofstream ofs(m_sFilesDir + "/" + sName, std::ios::binary);
        char buffer[16384];
        int nRead;
        size_t nTotal = 0;
        while((nRead = mg_read(pConn, buffer, sizeof(buffer))) > 0)
        {
            ofs.write(buffer, nRead);
            nTotal += static_cast<size_t>(nRead);
        }
        LOG_INFO("web") << "uploaded " << sName << " (" << nTotal/1024 << " kB)";
        SendJson(pConn, 200, {{"ok", true}, {"bytes", nTotal}});
    }
    else if(sUri.rfind("/api/action/", 0) == 0 && sMethod == "POST")
    {
        try
        {
            auto sBody = ReadBody(pConn);
            auto jsBody = sBody.empty() ? json::object() : json::parse(sBody);
            SendJson(pConn, 200, m_pAction ? m_pAction(sUri.substr(12), jsBody) : json::object());
        }
        catch(const std::exception& e)
        {
            SendJson(pConn, 400, {{"error", e.what()}});
        }
    }
    else
    {
        SendJson(pConn, 404, {{"error", "not found"}});
    }
    return 200;
}
