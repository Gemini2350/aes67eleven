#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pan::rtp
{
    struct SdpLeg
    {
        std::string sMid;           //a=mid (leg name, e.g. PRI/SEC)
        std::string sMulticast;
        std::string sSourceFilter;  //optional source specific address
        uint16_t nPort = 5004;
    };

    struct SdpSession
    {
        std::string sName;
        std::vector<SdpLeg> vLegs;  //1 = single, 2 = ST 2022-7 DUP
        int nPayloadType = 96;
        int nSampleRate = 48000;
        int nChannels = 2;
        int nBitsPerSample = 24;    //L24 or L16
        double dPacketTimeMs = 1.0;
        std::string sPtpGrandmaster;
        int nPtpDomain = 0;
    };

    std::optional<SdpSession> ParseSdp(const std::string& sSdp);
    std::string GenerateSdp(const SdpSession& session, const std::string& sOriginIp, const std::vector<std::string>& vSourceIps);
}
