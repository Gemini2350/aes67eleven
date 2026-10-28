#pragma once
#include <mutex>
#include <string>
#include "json.hpp"

namespace pan
{
    /** Json config persisted to disk. Thread safe. Defaults live in config.cpp. **/
    class Config
    {
        public:
            static Config& Get();

            bool Load(const std::string& sPath);
            void Save();

            nlohmann::json GetJson() const;

            // dotted path access, e.g. Get<int>("ptp.domain", 0)
            template<typename T> T GetValue(const std::string& sPath, const T& defaultValue) const
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                auto pointer = nlohmann::json::json_pointer("/"+Dotted(sPath));
                if(m_json.contains(pointer))
                {
                    try { return m_json.at(pointer).get<T>(); } catch(...) {}
                }
                return defaultValue;
            }

            template<typename T> void SetValue(const std::string& sPath, const T& value)
            {
                {
                    std::lock_guard<std::mutex> lg(m_mutex);
                    m_json[nlohmann::json::json_pointer("/"+Dotted(sPath))] = value;
                }
                Save();
            }

            // merge a json object of {dotted.path: value} updates
            void Update(const nlohmann::json& jsUpdates);

            // drop a top level key (one-time migrations)
            void Remove(const std::string& sKey);

        private:
            Config() = default;
            static std::string Dotted(const std::string& sPath);

            mutable std::mutex m_mutex;
            nlohmann::json m_json;
            std::string m_sPath;
    };
}
