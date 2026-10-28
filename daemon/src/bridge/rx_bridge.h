#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include "audio/sink.h"
#ifdef __APPLE__
#include "coreaudio/monitor_out.h"
#else
#include "linux/monitor_out.h"
#endif
#include "coreaudio/spsc_ring.h"
#include "driver_iface.h"

namespace aes67vsc
{
    /** One receiver's landing zone on "AES67 In": stream channel i lands on
    *   device channel map[i] (arbitrary matrix routing). The receiver's playout
    *   loop writes blocking (that backpressure paces it); the mixer's render
    *   pass pops and scatters the slice.
    **/
    class RxTap : public pan::audio::Sink
    {
        public:
            explicit RxTap(std::vector<uint32_t> vMap) :
                m_vMap(SanitizeMap(std::move(vMap))),
                m_ring(8192, m_vMap.size())
            {}

            //--- receiver side (audio::Sink) ---
            bool Write(const float* pInterleaved, size_t nFrames, size_t nChannels, double dGainDb) override
            {
                const size_t nWidth = m_vMap.size();
                float fGain = static_cast<float>(std::pow(10.0, dGainDb/20.0));
                if(m_vScratch.size() < nFrames*nWidth) { m_vScratch.resize(nFrames*nWidth); }
                size_t nCopy = std::min(nChannels, nWidth);
                for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
                {
                    float* pOut = m_vScratch.data() + nFrame*nWidth;
                    const float* pIn = pInterleaved + nFrame*nChannels;
                    size_t nCh = 0;
                    for(; nCh < nCopy; nCh++) { pOut[nCh] = pIn[nCh] * fGain; }
                    for(; nCh < nWidth; nCh++) { pOut[nCh] = 0.f; }
                }

                //block until there is room - the render pass drains at the ptp
                //rate. bounded so a detached device cannot deadlock the playout
                size_t nPushed = 0;
                for(int nWait = 0; m_bRun.load(std::memory_order_relaxed) && nWait < 1250; nWait++)
                {
                    nPushed += m_ring.PushFrames(m_vScratch.data() + nPushed*nWidth, nFrames - nPushed);
                    if(nPushed >= nFrames) { return true; }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                return false;
            }

            long DelayFrames() override { return static_cast<long>(m_ring.FillFrames()); }
            uint64_t Underruns() const override { return m_nUnderruns.load(); }

            //--- render side ---
            /** pop up to nFrames of this slice and scatter onto the device buffer **/
            void Render(float* pDeviceFrames, size_t nFrames)
            {
                const size_t nWidth = m_vMap.size();
                float aFrame[kMaxStreamChannels];
                for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
                {
                    if(m_ring.PopFrames(aFrame, 1) < 1)
                    {
                        if(nFrame == 0 && m_bExpectAudio.load(std::memory_order_relaxed)) { m_nUnderruns++; }
                        return;     //rest of the slice stays silent (pre-zeroed)
                    }
                    float* pOut = pDeviceFrames + nFrame*kChannels;
                    for(size_t nCh = 0; nCh < nWidth; nCh++)
                    {
                        pOut[m_vMap[nCh]] = aFrame[nCh];
                    }
                }
            }

            void SetExpectAudio(bool bExpect) { m_bExpectAudio.store(bExpect); }
            void Shutdown() { m_bRun.store(false); }
            const std::vector<uint32_t>& Map() const { return m_vMap; }

        private:
            static std::vector<uint32_t> SanitizeMap(std::vector<uint32_t> vMap)
            {
                if(vMap.empty()) { vMap = {0, 1}; }
                if(vMap.size() > kMaxStreamChannels) { vMap.resize(kMaxStreamChannels); }
                for(auto& nCh : vMap) { nCh = std::min(nCh, kChannels-1); }
                return vMap;
            }

            const std::vector<uint32_t> m_vMap;     //stream ch i -> device ch map[i]
            SpscRing m_ring;
            std::vector<float> m_vScratch;
            std::atomic<bool> m_bRun{true};
            std::atomic<bool> m_bExpectAudio{false};
            std::atomic<uint64_t> m_nUnderruns{0};
    };

    /** Composes the live receiver slices into the 64ch render buffer and feeds
    *   the local monitor. The io proc walks an immutable snapshot swapped
    *   atomically on topology changes.
    **/
    class RxMixer
    {
        public:
            using Taps = std::vector<std::shared_ptr<RxTap>>;

            RxMixer() : m_pTaps(std::make_shared<const Taps>()) {}

            void SetTaps(Taps vTaps)
            {
                std::atomic_store(&m_pTaps, std::shared_ptr<const Taps>(
                    std::make_shared<const Taps>(std::move(vTaps))));
            }

            //render io proc: fill the device buffer from every tap
            void Render(float* pDeviceFrames, size_t nFrames)
            {
                memset(pDeviceFrames, 0, nFrames * kChannels * sizeof(float));
                auto pTaps = std::atomic_load(&m_pTaps);
                for(const auto& pTap : *pTaps) { pTap->Render(pDeviceFrames, nFrames); }
                if(auto* pMonitor = m_pMonitor.load(std::memory_order_relaxed))
                {
                    pMonitor->Feed(pDeviceFrames, kChannels, nFrames);
                }
            }

            void SetMonitor(MonitorOut* pMonitor) { m_pMonitor.store(pMonitor); }

        private:
            std::shared_ptr<const Taps> m_pTaps;
            std::atomic<MonitorOut*> m_pMonitor{nullptr};
    };
}
