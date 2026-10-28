#pragma once
#include <string>
#include "json.hpp"

namespace aes67vsc
{
    /** gateway build: no local speakers - monitoring stays in the receiving
    *   devices. the stub keeps the config/ui plumbing alive. **/
    class MonitorOut
    {
        public:
            bool Start(const std::string&, double) { return false; }
            void Stop() {}
            void SetChannels(int, int) {}
            void Feed(const float*, size_t, size_t) {}
            nlohmann::json GetStatusJson() const
            {
                return {{"enabled", false}, {"running", false}, {"device", ""}, {"available", false}};
            }
    };
}
