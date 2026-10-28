#include "config.h"
#include <fstream>
#include "log.h"

using namespace pan;
using json = nlohmann::json;

namespace
{
    json Defaults()
    {
        return json{
            {"device", {
                {"label", "AES67eleven"},
                {"description", "AES67eleven - virtual soundcard with NMOS support"}
            }},
            {"network", {
                {"interface_primary", "en0"},
                {"interface_secondary", ""}         //empty = single leg operation
            }},
            {"ptp", {
                {"domain", 0},
                {"domain_auto", true},              //hop to the announced domain, manual = fallback
                {"announce_timeout_ms", 6000},
                {"delay_req_interval_ms", 1000}
            }},
            {"receiver", {
                {"enabled", true},
                {"playout_delay_ms", 20},
                {"gain_db", 0.0},
                {"monitor_left", -1},       //-1 = automatic (first pair)
                {"monitor_right", -1}
            }},
            //streams are dynamic: entries can be added/removed at runtime (web
            //api stream-add/stream-remove); "uid" keeps each stream's nmos ids
            //stable, "channel_map" routes stream channels onto the 64ch matrix
            {"senders", [] {
                json js = json::array();
                js.push_back({
                    {"uid", "legacy"},              //pre-v2 ids stay valid
                    {"label", ""},                  //empty = "<device label> sender 1"
                    {"enabled", false},
                    {"source", "device"},           //device | tone | sweep | file
                    {"tone_hz", 440.0},
                    {"tone_level_db", -18.0},
                    {"file", ""},
                    {"loop", true},
                    {"channel_offset", 0},
                    {"channels", 8},
                    {"bits", 24},
                    {"multicast_primary", "239.69.145.10"},
                    {"multicast_secondary", "239.69.146.10"},
                    {"leg1_enabled", true},
                    {"leg2_enabled", true},
                    {"port", 5004},
                    {"packet_time_us", 1000},
                    {"payload_type", 96},
                    {"multicast_loop", true}        //localhost testing needs it on
                });
                return js;
            }()},
            {"receivers", [] {
                json js = json::array();
                js.push_back({{"uid", "legacy"}, {"channel_offset", 0}, {"channels", 8}});
                return js;
            }()},
            {"monitor", {
                {"enabled", false},                 //off by default - opt in per receiver
                {"stream", 0},                      //which receiver is monitored
                {"device", "default"},              //default | device name | device uid
                {"gain_db", 0.0}
            }},
            {"nmos", {
                {"enabled", true},
                {"http_port", 8080},
                {"registry_override", ""}           //empty = discover via dns-sd
            }},
            {"web", {
                {"port", 8180},
                {"webui_path", "/usr/local/share/aes67-vsc/webui"},
                {"audio_files_path", ""}            //empty = ~/audio-files
            }}
        };
    }

    void MergeDefaults(json& jsTarget, const json& jsDefaults)
    {
        for(const auto& [sKey, jsValue] : jsDefaults.items())
        {
            if(!jsTarget.contains(sKey))
            {
                jsTarget[sKey] = jsValue;
            }
            else if(jsValue.is_object() && jsTarget[sKey].is_object())
            {
                MergeDefaults(jsTarget[sKey], jsValue);
            }
        }
    }
}

Config& Config::Get()
{
    static Config config;
    return config;
}

std::string Config::Dotted(const std::string& sPath)
{
    std::string s(sPath);
    for(auto& c : s)
    {
        if(c == '.') { c = '/'; }
    }
    return s;
}

bool Config::Load(const std::string& sPath)
{
    std::lock_guard<std::mutex> lg(m_mutex);
    m_sPath = sPath;
    m_json = json::object();
    std::ifstream ifs(sPath);
    if(ifs)
    {
        try { m_json = json::parse(ifs); }
        catch(const std::exception& e) { LOG_ERROR("config") << "parse failed: " << e.what(); }
    }
    MergeDefaults(m_json, Defaults());
    return true;
}

void Config::Save()
{
    std::lock_guard<std::mutex> lg(m_mutex);
    if(m_sPath.empty()) { return; }
    std::ofstream ofs(m_sPath);
    if(ofs) { ofs << m_json.dump(2) << "\n"; }
    else    { LOG_ERROR("config") << "cannot write " << m_sPath; }
}

json Config::GetJson() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_json;
}

void Config::Remove(const std::string& sKey)
{
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        m_json.erase(sKey);
    }
    Save();
}

void Config::Update(const json& jsUpdates)
{
    {
        std::lock_guard<std::mutex> lg(m_mutex);
        for(const auto& [sKey, jsValue] : jsUpdates.items())
        {
            m_json[json::json_pointer("/"+Dotted(sKey))] = jsValue;
        }
    }
    Save();
}
