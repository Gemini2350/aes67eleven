#pragma once
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "audio/meters.h"
#include "json.hpp"
#include "sdp.h"

namespace pan::ptp { class PtpClient; }
namespace pan::audio { class Sink; }

namespace pan::rtp
{
    /** AES67 receiver with ST 2022-7 seamless merge.
    *   One socket per leg, each bound to its own group address so a leg only
    *   ever sees its own stream (the macos stand-in for IP_MULTICAST_ALL).
    *   Packets from both legs land in one sequence-indexed jitter buffer -
    *   first arrival wins, the copy is dropped. Playout is anchored to the
    *   PTP media clock and then steered by the sink buffer level.
    **/
    class RtpReceiver
    {
        public:
            RtpReceiver(ptp::PtpClient& ptpClient, audio::Sink& sink) : m_ptp(ptpClient), m_pSink(&sink) {}
            ~RtpReceiver();

            /** swap the playout sink (only while stopped - the playout loop
            *   reads it without locks) **/
            void SetSink(audio::Sink& sink) { m_pSink = &sink; }

            /** legs in session order are joined on vInterfaces in the same order **/
            bool Configure(const SdpSession& session, const std::vector<std::string>& vInterfaces,
                           int nPlayoutDelayMs, std::function<double()> pGainDb);
            void Stop();

            bool IsRunning() const { return m_bRun.load(); }
            bool IsReceiving() const { return m_bReceiving.load(); }

            /** which source channel plays on the left/right ear (0 based),
            *   -1 = automatic (first pair). applies live, no restart **/
            void SetMonitor(int nLeft, int nRight)
            {
                m_nMonLeft = nLeft;
                m_nMonRight = nRight;
            }
            nlohmann::json GetStatusJson() const;
            audio::Meters& GetMeters() { return m_meters; }

            //bcp-008 counters
            uint64_t LostPackets(size_t nLeg) const { return nLeg < 2 ? m_aLegs[nLeg].nLost.load() : 0; }
            uint64_t LatePackets() const { return m_nConcealed.load(); }

        private:
            static constexpr size_t JITTER_SLOTS = 1024;

            struct Slot
            {
                std::atomic<uint32_t> nGeneration{0};   //0 = empty, else seq+1 marker
                uint16_t nSeq = 0;
                std::vector<float> vSamples;
            };

            struct LegState
            {
                int nSocket = -1;
                std::string sInterface;
                std::string sMulticast;
                std::atomic<uint64_t> nReceived{0};
                std::atomic<uint64_t> nLost{0};
                std::atomic<bool> bActive{false};
                uint16_t nLastSeq = 0;
                bool bHaveSeq = false;
                std::chrono::steady_clock::time_point lastPacket{};
            };

            void RxLoop();
            void PlayoutLoop();
            void HandlePacket(size_t nLeg, const uint8_t* pData, size_t nSize);

            //effective meter channels (same clamping as the status json)
            size_t MonitorLeft() const
            {
                size_t nCh = m_session.nChannels > 0 ? static_cast<size_t>(m_session.nChannels) : 1;
                int nMon = m_nMonLeft.load();
                return nMon >= 0 && static_cast<size_t>(nMon) < nCh ? static_cast<size_t>(nMon) : 0;
            }
            size_t MonitorRight() const
            {
                size_t nCh = m_session.nChannels > 0 ? static_cast<size_t>(m_session.nChannels) : 1;
                int nMon = m_nMonRight.load();
                return nMon >= 0 && static_cast<size_t>(nMon) < nCh ? static_cast<size_t>(nMon)
                       : (nCh >= 2 ? 1 : MonitorLeft());
            }

            ptp::PtpClient& m_ptp;
            audio::Sink* m_pSink;

            mutable std::mutex m_mutex;
            SdpSession m_session;
            std::array<LegState, 2> m_aLegs;
            size_t m_nLegCount = 0;
            int m_nPlayoutDelayMs = 20;
            std::function<double()> m_pGainDb;

            std::unique_ptr<Slot[]> m_vJitter = std::make_unique<Slot[]>(JITTER_SLOTS);   //atomics are not movable
            std::atomic<bool> m_bHaveFirst{false};
            std::atomic<uint16_t> m_nFirstSeq{0};
            std::atomic<uint32_t> m_nFirstTs{0};
            size_t m_nFramesPerPacket = 48;

            std::thread m_rxThread;
            std::thread m_playThread;
            std::atomic<bool> m_bRun{false};
            std::atomic<bool> m_bReceiving{false};
            std::atomic<uint64_t> m_nPlayed{0};
            std::atomic<uint64_t> m_nConcealed{0};
            std::atomic<uint64_t> m_nDuplicates{0};
            std::atomic<uint64_t> m_nMergedFromSecondary{0};
            std::atomic<int> m_nMonLeft{-1};
            std::atomic<int> m_nMonRight{-1};
            audio::Meters m_meters;

            //buffer level envelope for the ui chart: one sample per 100 ms
            //bucket with the min/max/last ms of audio buffered ahead of playout
            struct BufferSample { float fMin; float fMax; float fLast; };
            std::deque<BufferSample> m_qBufferHistory;      //guarded by m_mutex
    };
}
