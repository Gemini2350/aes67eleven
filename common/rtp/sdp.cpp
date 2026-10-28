#include "sdp.h"
#include <cstdio>
#include <sstream>

using namespace pan::rtp;

namespace
{
    std::string Trim(std::string s)
    {
        while(!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) { s.pop_back(); }
        return s;
    }
}

std::optional<SdpSession> pan::rtp::ParseSdp(const std::string& sSdp)
{
    SdpSession session;
    std::istringstream stream(sSdp);
    std::string sLine;
    std::string sSessionConnection;
    std::vector<std::string> vDupMids;
    SdpLeg* pCurrentLeg = nullptr;
    bool bInMedia = false;

    while(std::getline(stream, sLine))
    {
        sLine = Trim(sLine);
        if(sLine.size() < 2 || sLine[1] != '=') { continue; }
        auto sValue = sLine.substr(2);

        switch(sLine[0])
        {
            case 's':
                session.sName = sValue;
                break;
            case 'c':
            {
                //c=IN IP4 239.x.x.x/32
                auto nPos = sValue.rfind(' ');
                if(nPos == std::string::npos) { break; }
                auto sAddr = sValue.substr(nPos+1);
                if(auto nSlash = sAddr.find('/'); nSlash != std::string::npos) { sAddr = sAddr.substr(0, nSlash); }
                if(bInMedia && pCurrentLeg) { pCurrentLeg->sMulticast = sAddr; }
                else                        { sSessionConnection = sAddr; }
                break;
            }
            case 'm':
            {
                //m=audio 5004 RTP/AVP 96
                int nPort = 0, nPt = 96;
                if(sscanf(sValue.c_str(), "audio %d RTP/AVP %d", &nPort, &nPt) >= 1)
                {
                    session.vLegs.push_back({});
                    pCurrentLeg = &session.vLegs.back();
                    pCurrentLeg->nPort = static_cast<uint16_t>(nPort);
                    pCurrentLeg->sMulticast = sSessionConnection;
                    session.nPayloadType = nPt;
                    bInMedia = true;
                }
                break;
            }
            case 'a':
            {
                if(sValue.rfind("group:DUP ", 0) == 0)
                {
                    std::istringstream mids(sValue.substr(10));
                    std::string sMid;
                    while(mids >> sMid) { vDupMids.push_back(sMid); }
                }
                else if(sValue.rfind("mid:", 0) == 0 && pCurrentLeg)
                {
                    pCurrentLeg->sMid = sValue.substr(4);
                }
                else if(sValue.rfind("rtpmap:", 0) == 0)
                {
                    int nPt = 0, nBits = 0, nRate = 0, nChannels = 0;
                    int nMatched = sscanf(sValue.c_str(), "rtpmap:%d L%d/%d/%d", &nPt, &nBits, &nRate, &nChannels);
                    if(nMatched >= 3 && nPt == session.nPayloadType)
                    {
                        session.nBitsPerSample = nBits;
                        session.nSampleRate = nRate;
                        //rfc 3555: an omitted channel count means mono
                        session.nChannels = nMatched >= 4 && nChannels > 0 ? nChannels : 1;
                    }
                }
                else if(sValue.rfind("ptime:", 0) == 0)
                {
                    session.dPacketTimeMs = atof(sValue.c_str()+6);
                }
                else if(sValue.rfind("source-filter:", 0) == 0 && pCurrentLeg)
                {
                    //a=source-filter: incl IN IP4 <dest> <source>
                    auto nPos = sValue.rfind(' ');
                    if(nPos != std::string::npos) { pCurrentLeg->sSourceFilter = sValue.substr(nPos+1); }
                }
                else if(sValue.rfind("ts-refclk:ptp=", 0) == 0)
                {
                    //a=ts-refclk:ptp=IEEE1588-2008:xx-xx-..:domain
                    auto nColon = sValue.find(':', 14);
                    if(nColon != std::string::npos)
                    {
                        auto sRest = sValue.substr(nColon+1);
                        auto nDomainSep = sRest.rfind(':');
                        if(nDomainSep != std::string::npos)
                        {
                            session.sPtpGrandmaster = sRest.substr(0, nDomainSep);
                            session.nPtpDomain = atoi(sRest.c_str()+nDomainSep+1);
                        }
                        else
                        {
                            session.sPtpGrandmaster = sRest;
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    if(session.vLegs.empty()) { return std::nullopt; }

    //keep only the legs that are part of the DUP group, in group order
    if(vDupMids.size() > 1)
    {
        std::vector<SdpLeg> vOrdered;
        for(const auto& sMid : vDupMids)
        {
            for(const auto& leg : session.vLegs)
            {
                if(leg.sMid == sMid) { vOrdered.push_back(leg); }
            }
        }
        if(!vOrdered.empty()) { session.vLegs = vOrdered; }
    }
    if(session.vLegs.size() > 2) { session.vLegs.resize(2); }
    return session;
}

std::string pan::rtp::GenerateSdp(const SdpSession& session, const std::string& sOriginIp, const std::vector<std::string>& vSourceIps)
{
    std::ostringstream sdp;
    auto nSessId = static_cast<unsigned long>(time(nullptr));
    sdp << "v=0\r\n"
        << "o=- " << nSessId << " " << nSessId << " IN IP4 " << sOriginIp << "\r\n"
        << "s=" << session.sName << "\r\n"
        << "t=0 0\r\n";

    if(session.vLegs.size() > 1)
    {
        sdp << "a=group:DUP";
        for(const auto& leg : session.vLegs) { sdp << " " << leg.sMid; }
        sdp << "\r\n";
    }

    for(size_t nLeg = 0; nLeg < session.vLegs.size(); nLeg++)
    {
        const auto& leg = session.vLegs[nLeg];
        //per-leg source (set by the sender) wins over the positional list
        const auto& sSourceIp = !leg.sSourceFilter.empty() ? leg.sSourceFilter
                                : (nLeg < vSourceIps.size() ? vSourceIps[nLeg] : sOriginIp);
        sdp << "m=audio " << leg.nPort << " RTP/AVP " << session.nPayloadType << "\r\n"
            << "c=IN IP4 " << leg.sMulticast << "/32\r\n"
            << "a=source-filter: incl IN IP4 " << leg.sMulticast << " " << sSourceIp << "\r\n"
            << "a=rtpmap:" << session.nPayloadType << " L" << session.nBitsPerSample << "/"
                << session.nSampleRate << "/" << session.nChannels << "\r\n"
            << "a=ptime:" << session.dPacketTimeMs << "\r\n"
            << "a=recvonly\r\n";
        if(!session.sPtpGrandmaster.empty())
        {
            sdp << "a=ts-refclk:ptp=IEEE1588-2008:" << session.sPtpGrandmaster << ":" << session.nPtpDomain << "\r\n";
        }
        sdp << "a=mediaclk:direct=0\r\n";
        if(session.vLegs.size() > 1)
        {
            sdp << "a=mid:" << leg.sMid << "\r\n";
        }
    }
    return sdp.str();
}
