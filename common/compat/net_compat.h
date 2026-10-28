#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <sys/socket.h>

namespace pan::compat
{
    /** ipv4 address of an interface, "" if none **/
    std::string InterfaceIp(const std::string& sInterface);
    int InterfaceIndex(const std::string& sInterface);
    /** mac address for the EUI-64 style ptp clock identity **/
    std::array<uint8_t, 6> InterfaceMac(const std::string& sInterface);

    /** join a multicast group on a named interface (ip_mreq via the if address) **/
    bool JoinMulticast(int nSocket, const std::string& sGroup, const std::string& sInterface);
    /** outgoing multicast leaves on this interface **/
    bool SetMulticastEgress(int nSocket, const std::string& sInterface);

    /** bind to port; sAddress may be a multicast group - on macOS binding the
    *   group address is what isolates legs sharing a port (there is no
    *   IP_MULTICAST_ALL to switch off) **/
    bool BindUdp(int nSocket, uint16_t nPort, const std::string& sAddress = "");

    /** ask the kernel for receive timestamps + ingress interface **/
    void EnableRxInfo(int nSocket);
    /** kernel receive stamp mapped onto the host clock; nFallbackHostNs when absent.
    *   (SO_TIMESTAMP stamps against the wall clock - converted via the current
    *   realtime/host clock pair, adjacent reads keep the error in the µs range) **/
    uint64_t RxTimestampHostNs(msghdr& msg, uint64_t nFallbackHostNs);
    /** ingress interface index from IP_RECVIF, 0 when absent **/
    int RxIfIndex(msghdr& msg);
}
