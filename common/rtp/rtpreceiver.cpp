#include "rtpreceiver.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "audio/sink.h"
#include "compat/net_compat.h"
#include "compat/time_compat.h"
#include "log.h"
#include "ptp/ptpclient.h"

using namespace pan::rtp;
using json = nlohmann::json;

namespace
{
    constexpr int SAMPLE_RATE = 48000;

    int OpenLegSocket(const std::string& sInterface, const std::string& sMulticast, uint16_t nPort)
    {
        int nSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if(nSocket < 0) { return -1; }
        int nOn = 1;
        setsockopt(nSocket, SOL_SOCKET, SO_REUSEADDR, &nOn, sizeof(nOn));
        setsockopt(nSocket, SOL_SOCKET, SO_REUSEPORT, &nOn, sizeof(nOn));
        int nBuffer = 1 << 20;
        setsockopt(nSocket, SOL_SOCKET, SO_RCVBUF, &nBuffer, sizeof(nBuffer));

        //bind to the group address itself: on macos that is what keeps legs
        //sharing a port from seeing each other's stream
        if(!pan::compat::BindUdp(nSocket, nPort, sMulticast))
        {
            close(nSocket);
            return -1;
        }
        if(!pan::compat::JoinMulticast(nSocket, sMulticast, sInterface))
        {
            LOG_ERROR("rtprx") << "join " << sMulticast << " on " << sInterface << " failed";
            close(nSocket);
            return -1;
        }
        return nSocket;
    }
}

RtpReceiver::~RtpReceiver()
{
    Stop();
}

void RtpReceiver::Stop()
{
    m_bRun = false;
    if(m_rxThread.joinable()) { m_rxThread.join(); }
    if(m_playThread.joinable()) { m_playThread.join(); }
    for(auto& leg : m_aLegs)
    {
        if(leg.nSocket >= 0) { close(leg.nSocket); leg.nSocket = -1; }
        leg.bActive = false;
        leg.bHaveSeq = false;
    }
    m_bReceiving = false;
    m_bHaveFirst = false;
}

bool RtpReceiver::Configure(const SdpSession& session, const std::vector<std::string>& vInterfaces,
                            int nPlayoutDelayMs, std::function<double()> pGainDb)
{
    Stop();
    if(session.vLegs.empty() || vInterfaces.empty()) { return false; }

    std::lock_guard<std::mutex> lg(m_mutex);
    m_session = session;
    m_nPlayoutDelayMs = std::clamp(nPlayoutDelayMs, 2, 500);
    m_pGainDb = std::move(pGainDb);
    m_nLegCount = std::min(session.vLegs.size(), size_t(2));
    m_nFramesPerPacket = static_cast<size_t>(SAMPLE_RATE * session.dPacketTimeMs / 1000.0);
    if(m_nFramesPerPacket == 0) { m_nFramesPerPacket = 48; }

    //a leg whose interface is down is skipped with a warning - reception must
    //still run on the remaining leg (degraded 2022-7)
    size_t nUsable = 0;
    for(size_t nLeg = 0; nLeg < m_nLegCount; nLeg++)
    {
        auto& leg = m_aLegs[nUsable];
        leg.sInterface = nLeg < vInterfaces.size() && !vInterfaces[nLeg].empty()
                             ? vInterfaces[nLeg] : vInterfaces[0];
        leg.sMulticast = session.vLegs[nLeg].sMulticast;
        leg.nReceived = 0;
        leg.nLost = 0;
        leg.nSocket = OpenLegSocket(leg.sInterface, leg.sMulticast, session.vLegs[nLeg].nPort);
        if(leg.nSocket < 0)
        {
            LOG_WARN("rtprx") << "leg " << nLeg << " (" << leg.sMulticast << " on " << leg.sInterface
                              << ") not usable - leg skipped";
            continue;
        }
        LOG_INFO("rtprx") << "leg " << nUsable << ": " << leg.sMulticast << ":" << session.vLegs[nLeg].nPort
                          << " on " << leg.sInterface;
        nUsable++;
    }
    m_nLegCount = nUsable;
    if(m_nLegCount == 0)
    {
        LOG_ERROR("rtprx") << "no usable leg - receiver not started";
        return false;
    }

    for(size_t nSlot = 0; nSlot < JITTER_SLOTS; nSlot++)
    {
        m_vJitter[nSlot].nGeneration = 0;
    }
    m_bHaveFirst = false;
    m_nPlayed = 0;
    m_nConcealed = 0;
    m_nDuplicates = 0;
    m_nMergedFromSecondary = 0;
    m_qBufferHistory.clear();
    m_meters.Reset();

    m_bRun = true;
    m_rxThread = std::thread(&RtpReceiver::RxLoop, this);
    m_playThread = std::thread(&RtpReceiver::PlayoutLoop, this);
    return true;
}

void RtpReceiver::RxLoop()
{
    compat::SetRealtimeThread(1'000'000);       //arrivals must land in the slots first
    std::vector<uint8_t> vBuffer(4096);
    while(m_bRun)
    {
        fd_set fds;
        FD_ZERO(&fds);
        int nMax = -1;
        for(size_t nLeg = 0; nLeg < m_nLegCount; nLeg++)
        {
            FD_SET(m_aLegs[nLeg].nSocket, &fds);
            nMax = std::max(nMax, m_aLegs[nLeg].nSocket);
        }
        timeval tv{0, 100000};
        int nReady = select(nMax+1, &fds, nullptr, nullptr, &tv);

        //age out silent legs even while the other leg keeps us busy
        auto now = std::chrono::steady_clock::now();
        bool bAny = false;
        for(size_t nLeg = 0; nLeg < m_nLegCount; nLeg++)
        {
            if(now - m_aLegs[nLeg].lastPacket < std::chrono::milliseconds(500)) { bAny = true; }
            else { m_aLegs[nLeg].bActive = false; }
        }
        if(!bAny) { m_bReceiving = false; }
        if(nReady <= 0) { continue; }

        for(size_t nLeg = 0; nLeg < m_nLegCount; nLeg++)
        {
            if(!FD_ISSET(m_aLegs[nLeg].nSocket, &fds)) { continue; }
            auto nSize = recv(m_aLegs[nLeg].nSocket, vBuffer.data(), vBuffer.size(), 0);
            if(nSize > 12)
            {
                HandlePacket(nLeg, vBuffer.data(), static_cast<size_t>(nSize));
            }
        }
    }
}

void RtpReceiver::HandlePacket(size_t nLeg, const uint8_t* pData, size_t nSize)
{
    if((pData[0] & 0xc0) != 0x80) { return; }
    uint16_t nSeq = static_cast<uint16_t>((pData[2]<<8) | pData[3]);
    uint32_t nTs = (uint32_t(pData[4])<<24)|(uint32_t(pData[5])<<16)|(uint32_t(pData[6])<<8)|pData[7];

    auto& leg = m_aLegs[nLeg];
    leg.lastPacket = std::chrono::steady_clock::now();
    leg.bActive = true;
    leg.nReceived++;
    if(leg.bHaveSeq)
    {
        auto nDiff = static_cast<int16_t>(nSeq - leg.nLastSeq);
        if(nDiff > 1) { leg.nLost += static_cast<uint64_t>(nDiff - 1); }
    }
    leg.nLastSeq = nSeq;
    leg.bHaveSeq = true;
    m_bReceiving = true;

    //payload -> float frames, every stream channel decoded (the sink maps the
    //stream's channels onto the device)
    size_t nHeader = 12 + (pData[0] & 0x0f)*4;      //csrc list
    if(nSize <= nHeader) { return; }
    const uint8_t* pPayload = pData + nHeader;
    size_t nPayload = nSize - nHeader;
    size_t nBytesPerSample = m_session.nBitsPerSample == 16 ? 2 : 3;
    size_t nChannels = m_session.nChannels > 0 ? static_cast<size_t>(m_session.nChannels) : 1;
    size_t nFrames = nPayload / (nBytesPerSample * nChannels);
    if(nFrames == 0) { return; }

    auto& slot = m_vJitter[nSeq % JITTER_SLOTS];
    if(slot.nGeneration.load() == uint32_t(nSeq)+1)
    {
        m_nDuplicates++;        //other leg was first - seamless merge discard
        return;
    }

    auto Decode = [&](size_t nIndex) -> float
    {
        const uint8_t* p = pPayload + nIndex * nBytesPerSample;
        if(nBytesPerSample == 2)
        {
            return static_cast<int16_t>((p[0]<<8) | p[1]) / 32768.0f;
        }
        int32_t nSample = (int32_t(p[0])<<16) | (int32_t(p[1])<<8) | p[2];
        if(nSample & 0x800000) { nSample |= 0xff000000; }
        return nSample / 8388608.0f;
    };
    slot.vSamples.resize(nFrames * nChannels);
    for(size_t nIndex = 0; nIndex < nFrames * nChannels; nIndex++)
    {
        slot.vSamples[nIndex] = Decode(nIndex);
    }
    slot.nSeq = nSeq;
    slot.nGeneration = uint32_t(nSeq)+1;    //publish after the samples are in place
    if(nLeg == 1) { m_nMergedFromSecondary++; }

    if(!m_bHaveFirst.exchange(true))
    {
        m_nFirstSeq = nSeq;
        m_nFirstTs = nTs;
    }
}

void RtpReceiver::PlayoutLoop()
{
    compat::SetRealtimeThread(1'000'000);
    const size_t nChannels = m_session.nChannels > 0 ? static_cast<size_t>(m_session.nChannels) : 1;
    //wait for the first packet
    while(m_bRun && !m_bHaveFirst)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!m_bRun) { return; }

    //anchor: if the stream's rtp timestamps are on the ptp media clock, delay
    //playout so it happens exactly playout_delay after the sender's timestamp.
    //non-ptp streams (or no sync) just start with the configured buffer depth.
    if(m_ptp.IsSynced())
    {
        uint64_t nPtpNs = m_ptp.PtpTimeNs();
        uint64_t nPtpSamples = (nPtpNs / 1000000000ULL) * SAMPLE_RATE
                             + (nPtpNs % 1000000000ULL) * SAMPLE_RATE / 1000000000ULL;
        auto nAge = static_cast<int32_t>(static_cast<uint32_t>(nPtpSamples) - m_nFirstTs.load());
        int32_t nWaitSamples = m_nPlayoutDelayMs * SAMPLE_RATE / 1000 - nAge;
        if(nWaitSamples > 0 && nWaitSamples < SAMPLE_RATE)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(nWaitSamples) * 1000000 / SAMPLE_RATE));
        }
    }
    else
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_nPlayoutDelayMs));
    }

    uint16_t nNextSeq = m_nFirstSeq.load();
    std::vector<float> vSilence(m_nFramesPerPacket * nChannels, 0.f);
    const long nTargetDelayFrames = m_nPlayoutDelayMs * SAMPLE_RATE / 1000;

    //buffer level envelope: min/max within a 100 ms bucket -> history
    auto bucketStart = std::chrono::steady_clock::now();
    float fBufMin = 1e9f, fBufMax = 0.f;
    auto SampleBuffer = [&]()
    {
        float fMs = float(m_pSink->DelayFrames()) * 1000.0f / SAMPLE_RATE;
        fBufMin = std::min(fBufMin, fMs);
        fBufMax = std::max(fBufMax, fMs);
        auto now = std::chrono::steady_clock::now();
        if(now - bucketStart >= std::chrono::milliseconds(100))
        {
            bucketStart = now;
            std::lock_guard<std::mutex> lg(m_mutex);
            m_qBufferHistory.push_back({fBufMin, fBufMax, fMs});
            if(m_qBufferHistory.size() > 300) { m_qBufferHistory.pop_front(); }
            fBufMin = 1e9f;
            fBufMax = 0.f;
        }
    };

    while(m_bRun)
    {
        auto& slot = m_vJitter[nNextSeq % JITTER_SLOTS];
        double dGain = m_pGainDb ? m_pGainDb() : 0.0;

        if(slot.nGeneration.load() == uint32_t(nNextSeq)+1)
        {
            size_t nFrames = slot.vSamples.size()/nChannels;
            m_meters.Feed(slot.vSamples.data(), nFrames, nChannels, MonitorLeft(), MonitorRight());
            m_pSink->Write(slot.vSamples.data(), nFrames, nChannels, dGain);
            slot.nGeneration = 0;
            m_nPlayed++;
            nNextSeq++;
            SampleBuffer();
        }
        else if(m_bReceiving)
        {
            //not there yet - wait while the sink buffer still has audio queued
            if(m_pSink->DelayFrames() > long(m_nFramesPerPacket)*2)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(200));

                //jump forward if we stalled and newer packets are piling up
                bool bNewer = false;
                for(uint16_t nAhead = 1; nAhead <= 16; nAhead++)
                {
                    auto& probe = m_vJitter[uint16_t(nNextSeq+nAhead) % JITTER_SLOTS];
                    if(probe.nGeneration.load() == uint32_t(uint16_t(nNextSeq+nAhead))+1) { bNewer = true; break; }
                }
                if(bNewer && m_pSink->DelayFrames() < nTargetDelayFrames/2)
                {
                    //this seq was lost on every leg - conceal and move on
                    m_pSink->Write(vSilence.data(), m_nFramesPerPacket, nChannels, dGain);
                    m_nConcealed++;
                    nNextSeq++;
                    SampleBuffer();
                }
            }
            else
            {
                //buffer nearly dry - conceal to keep the sink fed
                m_pSink->Write(vSilence.data(), m_nFramesPerPacket, nChannels, dGain);
                m_nConcealed++;
                nNextSeq++;
                SampleBuffer();
            }
        }
        else
        {
            //stream silent/stopped - park and re-anchor on the next packet
            m_bHaveFirst = false;
            while(m_bRun && !m_bHaveFirst)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            nNextSeq = m_nFirstSeq.load();
        }

        //emergency drift recovery: with the device clock disciplined to ptp the
        //sink drains at exactly the stream rate, so this should never trigger -
        //it only catches holdover/relock edge cases
        if(m_pSink->DelayFrames() > nTargetDelayFrames + long(m_nFramesPerPacket)*4)
        {
            auto& ahead = m_vJitter[nNextSeq % JITTER_SLOTS];
            if(ahead.nGeneration.load() == uint32_t(nNextSeq)+1)
            {
                ahead.nGeneration = 0;
                nNextSeq++;     //drop one packet worth of audio
            }
        }
    }
}

json RtpReceiver::GetStatusJson() const
{
    json js;
    js["running"] = m_bRun.load();
    js["receiving"] = m_bReceiving.load();
    js["played"] = m_nPlayed.load();
    js["concealed"] = m_nConcealed.load();
    js["duplicates_merged"] = m_nDuplicates.load();
    js["from_secondary"] = m_nMergedFromSecondary.load();
    js["meters"] = {{"left_db", m_meters.LeftDb()}, {"right_db", m_meters.RightDb()}};

    std::lock_guard<std::mutex> lg(m_mutex);
    js["session_name"] = m_session.sName;
    js["channels"] = m_session.nChannels;
    js["bits"] = m_session.nBitsPerSample;
    js["playout_delay_ms"] = m_nPlayoutDelayMs;
    js["critical_ms"] = 2.0 * m_nFramesPerPacket * 1000.0 / 48000.0;    //below this the playout conceals
    {
        //effective monitor selection (same clamping as the packet decode)
        size_t nCh = m_session.nChannels > 0 ? static_cast<size_t>(m_session.nChannels) : 1;
        int nMonLeft = m_nMonLeft.load(), nMonRight = m_nMonRight.load();
        size_t nLeft = nMonLeft >= 0 && static_cast<size_t>(nMonLeft) < nCh ? nMonLeft : 0;
        size_t nRight = nMonRight >= 0 && static_cast<size_t>(nMonRight) < nCh ? nMonRight
                        : (nCh >= 2 ? 1 : nLeft);
        js["monitor"] = {nLeft, nRight};
    }
    js["buffer_history"] = json::array();
    for(const auto& sample : m_qBufferHistory)
    {
        js["buffer_history"].push_back({std::round(sample.fMin*10)/10.0, std::round(sample.fMax*10)/10.0,
                                        std::round(sample.fLast*10)/10.0});
    }
    js["legs"] = json::array();
    for(size_t nLeg = 0; nLeg < m_nLegCount; nLeg++)
    {
        const auto& leg = m_aLegs[nLeg];
        js["legs"].push_back({
            {"interface", leg.sInterface},
            {"multicast", leg.sMulticast},
            {"active", leg.bActive.load()},
            {"received", leg.nReceived.load()},
            {"lost", leg.nLost.load()}
        });
    }
    return js;
}
