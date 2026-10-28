#include "net_compat.h"
#include <arpa/inet.h>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#else
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <netinet/in.h>
#include <sys/time.h>
#include "time_compat.h"

using namespace pan::compat;

std::string pan::compat::InterfaceIp(const std::string& sInterface)
{
    ifaddrs* pList = nullptr;
    if(getifaddrs(&pList) != 0) { return ""; }
    std::string sIp;
    for(auto* p = pList; p; p = p->ifa_next)
    {
        if(p->ifa_addr && p->ifa_addr->sa_family == AF_INET && sInterface == p->ifa_name)
        {
            char buffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buffer, sizeof(buffer));
            sIp = buffer;
            break;
        }
    }
    freeifaddrs(pList);
    return sIp;
}

int pan::compat::InterfaceIndex(const std::string& sInterface)
{
    return static_cast<int>(if_nametoindex(sInterface.c_str()));
}

std::array<uint8_t, 6> pan::compat::InterfaceMac(const std::string& sInterface)
{
    std::array<uint8_t, 6> mac{};
#ifndef __APPLE__
    int nSock = socket(AF_INET, SOCK_DGRAM, 0);
    if(nSock >= 0)
    {
        ifreq ifr{};
        strncpy(ifr.ifr_name, sInterface.c_str(), IFNAMSIZ-1);
        if(ioctl(nSock, SIOCGIFHWADDR, &ifr) == 0)
        {
            memcpy(mac.data(), ifr.ifr_hwaddr.sa_data, 6);
        }
        close(nSock);
    }
    return mac;
#else
    ifaddrs* pList = nullptr;
    if(getifaddrs(&pList) != 0) { return mac; }
    for(auto* p = pList; p; p = p->ifa_next)
    {
        if(p->ifa_addr && p->ifa_addr->sa_family == AF_LINK && sInterface == p->ifa_name)
        {
            auto* pDl = reinterpret_cast<sockaddr_dl*>(p->ifa_addr);
            if(pDl->sdl_alen == 6)
            {
                memcpy(mac.data(), LLADDR(pDl), 6);
            }
            break;
        }
    }
    freeifaddrs(pList);
    return mac;
#endif
}

bool pan::compat::JoinMulticast(int nSocket, const std::string& sGroup, const std::string& sInterface)
{
    auto sIfIp = InterfaceIp(sInterface);
    ip_mreq mreq{};
    inet_pton(AF_INET, sGroup.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = sIfIp.empty() ? htonl(INADDR_ANY) : inet_addr(sIfIp.c_str());
    return setsockopt(nSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
}

bool pan::compat::SetMulticastEgress(int nSocket, const std::string& sInterface)
{
    auto sIfIp = InterfaceIp(sInterface);
    if(sIfIp.empty()) { return false; }
    in_addr addr{};
    addr.s_addr = inet_addr(sIfIp.c_str());
    return setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr)) == 0;
}

bool pan::compat::BindUdp(int nSocket, uint16_t nPort, const std::string& sAddress)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(nPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(!sAddress.empty())
    {
        inet_pton(AF_INET, sAddress.c_str(), &addr.sin_addr);
    }
#ifdef IP_MULTICAST_ALL
    //linux: without this a socket joined to group A also receives group B
    //when another socket on the same port joined B - breaks leg isolation
    int nOff = 0;
    setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_ALL, &nOff, sizeof(nOff));
#endif
    return bind(nSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

void pan::compat::EnableRxInfo(int nSocket)
{
    int nOn = 1;
    setsockopt(nSocket, SOL_SOCKET, SO_TIMESTAMP, &nOn, sizeof(nOn));
#ifdef __APPLE__
    setsockopt(nSocket, IPPROTO_IP, IP_RECVIF, &nOn, sizeof(nOn));
#else
    setsockopt(nSocket, IPPROTO_IP, IP_PKTINFO, &nOn, sizeof(nOn));
#endif
}

uint64_t pan::compat::RxTimestampHostNs(msghdr& msg, uint64_t nFallbackHostNs)
{
    for(cmsghdr* pCmsg = CMSG_FIRSTHDR(&msg); pCmsg; pCmsg = CMSG_NXTHDR(&msg, pCmsg))
    {
        if(pCmsg->cmsg_level == SOL_SOCKET && pCmsg->cmsg_type == SCM_TIMESTAMP)
        {
            timeval tv;
            memcpy(&tv, CMSG_DATA(pCmsg), sizeof(tv));
            uint64_t nStampRealtime = uint64_t(tv.tv_sec)*1000000000ULL + uint64_t(tv.tv_usec)*1000ULL;
            //move the wall clock stamp onto the host clock via an adjacent read pair
            uint64_t nNowRealtime = RealtimeNs();
            uint64_t nNowHost = HostNs();
            return nNowHost - (nNowRealtime - nStampRealtime);
        }
    }
    return nFallbackHostNs;
}

int pan::compat::RxIfIndex(msghdr& msg)
{
    for(cmsghdr* pCmsg = CMSG_FIRSTHDR(&msg); pCmsg; pCmsg = CMSG_NXTHDR(&msg, pCmsg))
    {
#ifdef __APPLE__
        if(pCmsg->cmsg_level == IPPROTO_IP && pCmsg->cmsg_type == IP_RECVIF)
        {
            sockaddr_dl dl;
            memcpy(&dl, CMSG_DATA(pCmsg), std::min(sizeof(dl), size_t(pCmsg->cmsg_len - CMSG_LEN(0))));
            return dl.sdl_index;
        }
#else
        if(pCmsg->cmsg_level == IPPROTO_IP && pCmsg->cmsg_type == IP_PKTINFO)
        {
            in_pktinfo info;
            memcpy(&info, CMSG_DATA(pCmsg), sizeof(info));
            return info.ipi_ifindex;
        }
#endif
    }
    return 0;
}
