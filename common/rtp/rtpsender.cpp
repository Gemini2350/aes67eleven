#include "rtpsender.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>
#include "compat/net_compat.h"
#include "compat/time_compat.h"
#include "config.h"
#include "log.h"
#include "ptp/ptpclient.h"

using namespace pan::rtp;
using json = nlohmann::json;

namespace
{
    constexpr int SAMPLE_RATE = 48000;
}

RtpSender::~RtpSender()
{
    Stop();
}

void RtpSender::Stop()
{
    m_bRun = false;
    if(m_thread.joinable()) { m_thread.join(); }
    std::lock_guard<std::mutex> lg(m_mutex);
    for(int nSocket : m_vSockets) { close(nSocket); }
    m_vSockets.clear();
    m_bEssenceOk = false;
}

bool RtpSender::Configure(const std::vector<Leg>& vLegs, uint16_t nPort, int nPayloadType,
                          int nPacketTimeUs, int nChannels, int nBitsPerSample,
                          const std::string& sLabel, bool bMulticastLoop,
                          std::unique_ptr<audio::Source> pSource)
{
    Stop();
    if(vLegs.empty() || !pSource) { return false; }

    std::lock_guard<std::mutex> lg(m_mutex);
    m_vLegs.clear();
    m_nPort = nPort;
    m_nPayloadType = nPayloadType;
    m_nPacketTimeUs = std::max(125, nPacketTimeUs);
    m_nChannels = std::clamp(nChannels, 1, 8);      //aes67 mtu at 48k/1ms/L24
    m_nBitsPerSample = nBitsPerSample == 16 ? 16 : 24;
    m_sLabel = sLabel;
    m_bMulticastLoop = bMulticastLoop;
    m_pSource = std::move(pSource);
    m_sSourceName = m_pSource->Describe();
    m_vSourceIps.clear();

    //a leg whose interface is down (unplugged dock etc.) is skipped with a
    //warning - the stream must still go out on the remaining leg
    for(const auto& leg : vLegs)
    {
        int nSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if(nSocket < 0) { continue; }

        if(!compat::SetMulticastEgress(nSocket, leg.sInterface))
        {
            LOG_WARN("rtptx") << "leg interface " << leg.sInterface << " not usable - leg skipped";
            close(nSocket);
            continue;
        }
        uint8_t nTtl = 64;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_TTL, &nTtl, sizeof(nTtl));
        //loopback on lets a receiver on this same mac hear us (single-machine tests)
        uint8_t nLoop = m_bMulticastLoop ? 1 : 0;
        setsockopt(nSocket, IPPROTO_IP, IP_MULTICAST_LOOP, &nLoop, sizeof(nLoop));
        int nTos = 0xB8;    //EF - AES67 media
        setsockopt(nSocket, IPPROTO_IP, IP_TOS, &nTos, sizeof(nTos));

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(m_nPort);
        inet_pton(AF_INET, leg.sMulticast.c_str(), &dest.sin_addr);
        if(connect(nSocket, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) < 0)
        {
            LOG_WARN("rtptx") << "leg " << leg.sMulticast << " not connectable - leg skipped";
            close(nSocket);
            continue;
        }
        m_vLegs.push_back(leg);
        m_vSockets.push_back(nSocket);
        m_vSourceIps.push_back(compat::InterfaceIp(leg.sInterface));
        m_abLegEnabled[m_vSockets.size()-1] = leg.bEnabled;
    }
    if(m_vSockets.empty())
    {
        LOG_ERROR("rtptx") << "no usable leg - sender not started";
        return false;
    }

    std::random_device rd;
    m_nSsrc = rd();
    m_nPacketsSent = 0;
    m_nSendErrors = 0;
    m_meters.Reset();

    m_bRun = true;
    m_thread = std::thread(&RtpSender::SendLoop, this);
    LOG_INFO("rtptx") << "sending '" << m_sSourceName << "' to "
                      << m_vLegs[0].sMulticast << ":" << m_nPort
                      << (m_vLegs.size() > 1 ? " + "+m_vLegs[1].sMulticast+" (2022-7)" : "")
                      << " ptime " << m_nPacketTimeUs << "us";
    return true;
}

void RtpSender::SendLoop()
{
    //pacing is time critical - move onto the mach time-constraint band
    compat::SetRealtimeThread(uint64_t(m_nPacketTimeUs) * 1000);

    const size_t nChannels = static_cast<size_t>(m_nChannels);
    const size_t nBytesPerSample = m_nBitsPerSample == 16 ? 2 : 3;
    const size_t nFramesPerPacket = static_cast<size_t>(SAMPLE_RATE) * m_nPacketTimeUs / 1000000;
    const size_t nPayloadBytes = nFramesPerPacket * nChannels * nBytesPerSample;
    std::vector<float> vAudio(nFramesPerPacket * nChannels, 0.f);

    //sources narrower than the stream feed the first channels, the rest is silence
    const size_t nSourceChannels = m_pSource ? m_pSource->Channels() : 2;
    std::vector<float> vSourceAudio(nFramesPerPacket * nSourceChannels);
    const bool bMapSource = nSourceChannels != nChannels;

    std::vector<uint8_t> vPacket(12 + nPayloadBytes);

    //sleep to just before the deadline, spin the last stretch - the absolute
    //wakeup alone is only good to a few tens of microseconds. capped to a
    //quarter of the packet time so tiny ptimes do not turn into busy loops
    const int64_t SPIN_NS = std::min<int64_t>(100'000, static_cast<int64_t>(m_nPacketTimeUs) * 250);

    //anchor: packet n leaves at ptp time nStartNs + n*ptime, rtp timestamp is
    //the ptp media clock (samples since ptp epoch, mediaclk:direct=0)
    uint64_t nStartNs = 0;
    uint64_t nPacket = 0;
    uint64_t nWindowEndNs = 0;
    int64_t nLateAvgNs = 0, nLateMaxNs = 0;
    uint16_t nSeq = static_cast<uint16_t>(m_nSsrc);
    bool bNeedAnchor = true;

    while(m_bRun)
    {
        //(re)anchor whenever ptp is not usable or the mapping jumped
        if(bNeedAnchor || !m_ptp.IsSynced())
        {
            m_bWaitingForPtp = true;
            while(m_bRun && !m_ptp.IsSynced())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            m_bWaitingForPtp = false;
            if(!m_bRun) { return; }
            nStartNs = m_ptp.PtpTimeNs() + 20'000'000;      //begin 20 ms in the future
            nPacket = 0;
            nWindowEndNs = nStartNs + 5'000'000'000ULL;
            bNeedAnchor = false;
        }

        uint64_t nDeadlineNs = nStartNs + nPacket * static_cast<uint64_t>(m_nPacketTimeUs) * 1000;
        int64_t nAheadNs = static_cast<int64_t>(nDeadlineNs) - static_cast<int64_t>(m_ptp.PtpTimeNs());
        if(nAheadNs > 2'000'000'000 || nAheadNs < -2'000'000'000)
        {
            bNeedAnchor = true;     //mapping jumped (ptp restart)
            continue;
        }

        //build the whole packet BEFORE the deadline so the wakeup only sends.
        //no lock: source and sockets only change while the thread is stopped
        //(Configure joins first), the leg switches are atomics
        bool bHaveAudio;
        if(bMapSource)
        {
            bHaveAudio = m_pSource && m_pSource->Read(vSourceAudio.data(), nFramesPerPacket);
            size_t nCopy = std::min(nSourceChannels, nChannels);
            for(size_t nFrame = 0; nFrame < nFramesPerPacket; nFrame++)
            {
                for(size_t nCh = 0; nCh < nCopy; nCh++)
                {
                    vAudio[nFrame*nChannels + nCh] = vSourceAudio[nFrame*nSourceChannels + nCh];
                }
            }
        }
        else
        {
            bHaveAudio = m_pSource && m_pSource->Read(vAudio.data(), nFramesPerPacket);
        }
        m_bEssenceOk = bHaveAudio;
        m_meters.Feed(vAudio.data(), nFramesPerPacket, nChannels, 0, 1);

        //rtp header - media clock is samples since the ptp epoch mod 2^32
        //(mediaclk:direct=0). split seconds off first: ns*48000 overflows uint64
        uint64_t nSec = nDeadlineNs / 1000000000ULL;
        uint32_t nRtpTs = static_cast<uint32_t>(nSec * SAMPLE_RATE
                            + (nDeadlineNs % 1000000000ULL) * SAMPLE_RATE / 1000000000ULL);
        vPacket[0] = 0x80;
        vPacket[1] = static_cast<uint8_t>(m_nPayloadType);
        vPacket[2] = nSeq >> 8;
        vPacket[3] = nSeq & 0xff;
        vPacket[4] = nRtpTs >> 24; vPacket[5] = (nRtpTs >> 16) & 0xff;
        vPacket[6] = (nRtpTs >> 8) & 0xff; vPacket[7] = nRtpTs & 0xff;
        vPacket[8] = m_nSsrc >> 24; vPacket[9] = (m_nSsrc >> 16) & 0xff;
        vPacket[10] = (m_nSsrc >> 8) & 0xff; vPacket[11] = m_nSsrc & 0xff;

        //L24/L16 big endian payload
        if(nBytesPerSample == 3)
        {
            for(size_t i = 0; i < nFramesPerPacket * nChannels; i++)
            {
                float f = std::clamp(vAudio[i], -1.0f, 1.0f);
                auto nSample = static_cast<int32_t>(f * 8388607.0f);
                vPacket[12 + i*3] = (nSample >> 16) & 0xff;
                vPacket[12 + i*3 + 1] = (nSample >> 8) & 0xff;
                vPacket[12 + i*3 + 2] = nSample & 0xff;
            }
        }
        else
        {
            for(size_t i = 0; i < nFramesPerPacket * nChannels; i++)
            {
                float f = std::clamp(vAudio[i], -1.0f, 1.0f);
                auto nSample = static_cast<int16_t>(f * 32767.0f);
                vPacket[12 + i*2] = (nSample >> 8) & 0xff;
                vPacket[12 + i*2 + 1] = nSample & 0xff;
            }
        }

        //absolute sleep on the host clock (deadline via the ptp mapping),
        //then spin the final stretch to the exact ptp deadline
        compat::SleepUntilHostNs(m_ptp.PtpToHostNs(nDeadlineNs) - SPIN_NS);
        for(;;)
        {
            int64_t nRemainNs = static_cast<int64_t>(nDeadlineNs) - static_cast<int64_t>(m_ptp.PtpTimeNs());
            if(nRemainNs <= 0 || nRemainNs > 2*SPIN_NS) { break; }  //due, or the mapping moved
        }

        for(size_t nLeg = 0; nLeg < m_vSockets.size(); nLeg++)
        {
            if(!m_abLegEnabled[nLeg]) { continue; }
            if(send(m_vSockets[nLeg], vPacket.data(), vPacket.size(), 0) < 0)
            {
                m_nSendErrors++;
            }
        }
        m_nPacketsSent++;

        //pacing quality: lateness of this send vs its deadline
        int64_t nLateNs = std::max<int64_t>(0,
            static_cast<int64_t>(m_ptp.PtpTimeNs()) - static_cast<int64_t>(nDeadlineNs));
        nLateAvgNs += (nLateNs - nLateAvgNs) / 16;
        nLateMaxNs = std::max(nLateMaxNs, nLateNs);
        m_nLateAvgNs = nLateAvgNs;
        if(nDeadlineNs >= nWindowEndNs)
        {
            m_nLateMaxNs = nLateMaxNs;
            nLateMaxNs = 0;
            nWindowEndNs = nDeadlineNs + 5'000'000'000ULL;
        }
        nSeq++;
        nPacket++;
    }
}

SdpSession RtpSender::DescribeSession() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    SdpSession session;
    session.sName = m_sLabel.empty() ? "AES67eleven " + m_sSourceName : m_sLabel;
    session.nPayloadType = m_nPayloadType;
    session.nChannels = m_nChannels;
    session.nBitsPerSample = m_nBitsPerSample;
    session.dPacketTimeMs = m_nPacketTimeUs / 1000.0;
    session.sPtpGrandmaster = m_ptp.GrandmasterId();
    //only enabled legs go into the sdp - receivers must not join a silent group
    size_t nEnabled = 0;
    for(size_t nLeg = 0; nLeg < m_vLegs.size(); nLeg++) { if(m_abLegEnabled[nLeg]) { nEnabled++; } }
    for(size_t nLeg = 0; nLeg < m_vLegs.size(); nLeg++)
    {
        if(!m_abLegEnabled[nLeg]) { continue; }
        session.vLegs.push_back({nEnabled > 1 ? (nLeg == 0 ? "PRI" : "SEC") : "",
                                 m_vLegs[nLeg].sMulticast,
                                 nLeg < m_vSourceIps.size() ? m_vSourceIps[nLeg] : "",
                                 m_nPort});
    }
    return session;
}

std::vector<std::string> RtpSender::GetSourceIps() const
{
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_vSourceIps;
}

json RtpSender::GetStatusJson() const
{
    json js;
    js["running"] = m_bRun.load();
    js["waiting_for_ptp"] = m_bWaitingForPtp.load();
    js["essence_ok"] = m_bEssenceOk.load();
    js["packets_sent"] = m_nPacketsSent.load();
    js["send_errors"] = m_nSendErrors.load();
    js["late_avg_us"] = m_nLateAvgNs.load() / 1000.0;
    js["late_max_us"] = m_nLateMaxNs.load() / 1000.0;
    js["channels"] = m_nChannels;
    js["bits"] = m_nBitsPerSample;
    js["meters"] = {{"left_db", m_meters.LeftDb()}, {"right_db", m_meters.RightDb()}};
    std::lock_guard<std::mutex> lg(m_mutex);
    js["source"] = m_sSourceName;
    js["legs"] = json::array();
    for(size_t nLeg = 0; nLeg < m_vLegs.size(); nLeg++)
    {
        js["legs"].push_back({{"interface", m_vLegs[nLeg].sInterface}, {"multicast", m_vLegs[nLeg].sMulticast},
                              {"port", m_nPort}, {"enabled", m_abLegEnabled[nLeg].load()}});
    }
    return js;
}
