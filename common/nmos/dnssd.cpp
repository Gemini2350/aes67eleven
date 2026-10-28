#include "dnssd.h"
#include <algorithm>
#include <arpa/nameser.h>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <resolv.h>
#include <sstream>
#include "log.h"

using namespace pan::nmos;

namespace
{
    std::vector<std::string> SearchDomains()
    {
        std::vector<std::string> vDomains;
        std::ifstream ifs("/etc/resolv.conf");
        std::string sLine;
        while(std::getline(ifs, sLine))
        {
            std::istringstream iss(sLine);
            std::string sKey;
            iss >> sKey;
            if(sKey == "search" || sKey == "domain")
            {
                std::string sDomain;
                while(iss >> sDomain) { vDomains.push_back(sDomain); }
            }
        }
        return vDomains;
    }

    struct SrvResult
    {
        std::string sTarget;
        uint16_t nPort = 0;
    };

    std::vector<std::string> QueryPtr(const std::string& sName)
    {
        std::vector<std::string> vTargets;
        unsigned char answer[4096];
        int nSize = res_query(sName.c_str(), ns_c_in, ns_t_ptr, answer, sizeof(answer));
        if(nSize <= 0) { return vTargets; }

        ns_msg msg;
        if(ns_initparse(answer, nSize, &msg) != 0) { return vTargets; }
        for(int i = 0; i < ns_msg_count(msg, ns_s_an); i++)
        {
            ns_rr rr;
            if(ns_parserr(&msg, ns_s_an, i, &rr) != 0 || ns_rr_type(rr) != ns_t_ptr) { continue; }
            char sTarget[NS_MAXDNAME];
            if(ns_name_uncompress(ns_msg_base(msg), ns_msg_end(msg), ns_rr_rdata(rr), sTarget, sizeof(sTarget)) > 0)
            {
                vTargets.emplace_back(sTarget);
            }
        }
        return vTargets;
    }

    SrvResult QuerySrv(const std::string& sName)
    {
        SrvResult result;
        unsigned char answer[4096];
        int nSize = res_query(sName.c_str(), ns_c_in, ns_t_srv, answer, sizeof(answer));
        if(nSize <= 0) { return result; }
        ns_msg msg;
        if(ns_initparse(answer, nSize, &msg) != 0) { return result; }
        for(int i = 0; i < ns_msg_count(msg, ns_s_an); i++)
        {
            ns_rr rr;
            if(ns_parserr(&msg, ns_s_an, i, &rr) != 0 || ns_rr_type(rr) != ns_t_srv) { continue; }
            const unsigned char* pData = ns_rr_rdata(rr);
            result.nPort = static_cast<uint16_t>((pData[4]<<8)|pData[5]);
            char sTarget[NS_MAXDNAME];
            if(ns_name_uncompress(ns_msg_base(msg), ns_msg_end(msg), pData+6, sTarget, sizeof(sTarget)) > 0)
            {
                result.sTarget = sTarget;
            }
            break;
        }
        return result;
    }

    std::vector<std::string> QueryTxt(const std::string& sName)
    {
        std::vector<std::string> vStrings;
        unsigned char answer[4096];
        int nSize = res_query(sName.c_str(), ns_c_in, ns_t_txt, answer, sizeof(answer));
        if(nSize <= 0) { return vStrings; }
        ns_msg msg;
        if(ns_initparse(answer, nSize, &msg) != 0) { return vStrings; }
        for(int i = 0; i < ns_msg_count(msg, ns_s_an); i++)
        {
            ns_rr rr;
            if(ns_parserr(&msg, ns_s_an, i, &rr) != 0 || ns_rr_type(rr) != ns_t_txt) { continue; }
            const unsigned char* pData = ns_rr_rdata(rr);
            size_t nRemaining = ns_rr_rdlen(rr);
            while(nRemaining > 0)
            {
                size_t nLen = *pData;
                if(nLen+1 > nRemaining) { break; }
                vStrings.emplace_back(reinterpret_cast<const char*>(pData+1), nLen);
                pData += nLen+1;
                nRemaining -= nLen+1;
            }
        }
        return vStrings;
    }
}

std::vector<RegistryEntry> pan::nmos::DiscoverRegistries()
{
    std::vector<RegistryEntry> vRegistries;
    res_init();     //re-read resolv.conf every discovery - no stale nameservers

    for(const auto& sDomain : SearchDomains())
    {
        for(const char* sService : {"_nmos-register._tcp.", "_nmos-registration._tcp."})
        {
            for(const auto& sInstance : QueryPtr(sService + sDomain))
            {
                auto srv = QuerySrv(sInstance);
                if(srv.sTarget.empty() || srv.nPort == 0) { continue; }

                RegistryEntry entry;
                entry.nPriority = 100;
                std::string sProto = "http";
                for(const auto& sTxt : QueryTxt(sInstance))
                {
                    if(sTxt.rfind("pri=", 0) == 0) { entry.nPriority = atoi(sTxt.c_str()+4); }
                    if(sTxt.rfind("api_proto=", 0) == 0) { sProto = sTxt.substr(10); }
                    if(sTxt.rfind("api_ver=", 0) == 0) { entry.sApiVersions = sTxt.substr(8); }
                }
                if(!srv.sTarget.empty() && srv.sTarget.back() == '.') { srv.sTarget.pop_back(); }
                entry.sUrl = sProto + "://" + srv.sTarget + ":" + std::to_string(srv.nPort);

                if(std::none_of(vRegistries.begin(), vRegistries.end(),
                                [&entry](const auto& e){ return e.sUrl == entry.sUrl; }))
                {
                    vRegistries.push_back(entry);
                    LOG_INFO("dnssd") << "registry " << entry.sUrl << " (pri " << entry.nPriority << ")";
                }
            }
        }
    }
    std::sort(vRegistries.begin(), vRegistries.end(),
              [](const auto& a, const auto& b){ return a.nPriority < b.nPriority; });
    return vRegistries;
}
