#pragma once
#include <string>
#include <vector>

namespace pan::nmos
{
    struct RegistryEntry
    {
        std::string sUrl;       //http://host:port  (no trailing path)
        int nPriority = 100;
        std::string sApiVersions;
    };

    /** Unicast DNS-SD (RFC 6763 over plain dns): query the resolv.conf search
    *   domains for _nmos-register._tcp PTR -> SRV/TXT/A and return every
    *   registration api found, best priority first. No avahi involved, so no
    *   stale-daemon-cache surprises.
    **/
    std::vector<RegistryEntry> DiscoverRegistries();
}
