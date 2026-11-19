#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "audio/sources.h"
#include "coreaudio/spsc_ring.h"
#include "driver_iface.h"

namespace aes67vsc
{
    /** One sender's pick of the capture device: an arbitrary channel map
    *   (stream channel i <- device channel map[i]) out of "AES67 Out". The
    *   capture drain pushes via the fanout, the sender's ptp-paced loop pulls
    *   through the audio::Source interface. Both sides tick on the
    *   ptp-disciplined device clock, but the drain delivers in chunks whose
    *   size and timing depend on the host (a hal property read through
    *   coreaudiod takes anything from under 1 ms to 10 ms) - the cushion
    *   absorbs that.
    **/
    class TxTap : public pan::audio::Source
    {
        public:
            explicit TxTap(std::vector<uint32_t> vMap) :
                m_vMap(SanitizeMap(std::move(vMap))),
                m_ring(8192, m_vMap.size()),
                m_vSlice(256 * m_vMap.size())
            {}

            //--- producer side (capture drain thread) ---
            void Push(const float* pDeviceFrames, size_t nFrames)
            {
                const size_t nCount = m_vMap.size();
                const size_t nCushion = m_nCushion.load(std::memory_order_relaxed);
                const size_t nBefore = m_ring.FillFrames();
                if(nCushion > 0)
                {
                    if(nBefore == 0)
                    {
                        //the reader ran dry (or audio just started): lead with
                        //silence, so the next late chunk finds frames left.
                        //each dry run adds one cushion - the ring grows to
                        //whatever the delivery jitter of this host needs
                        std::fill(m_vSlice.begin(), m_vSlice.end(), 0.0f);
                        for(size_t nDone = 0; nDone < nCushion;)
                        {
                            size_t nChunk = std::min(nCushion - nDone, m_vSlice.size()/nCount);
                            m_ring.PushFrames(m_vSlice.data(), nChunk);
                            nDone += nChunk;
                        }
                        m_nPrimed++;
                        m_nTrough = SIZE_MAX;
                        m_nWindow = 0;
                    }
                    else
                    {
                        m_nTrough = std::min(m_nTrough, nBefore);
                    }
                }
                for(size_t nDone = 0; nDone < nFrames;)
                {
                    size_t nChunk = std::min(nFrames - nDone, m_vSlice.size()/nCount);
                    for(size_t nFrame = 0; nFrame < nChunk; nFrame++)
                    {
                        const float* pSrc = pDeviceFrames + (nDone+nFrame)*kChannels;
                        float* pDst = m_vSlice.data() + nFrame*nCount;
                        for(size_t nCh = 0; nCh < nCount; nCh++)
                        {
                            pDst[nCh] = pSrc[m_vMap[nCh]];
                        }
                    }
                    m_nPushed += m_ring.PushFrames(m_vSlice.data(), nChunk);
                    nDone += nChunk;
                }
                //hard bound on the latency. the ring dwell does not add link
                //latency, rtp timestamps are taken at read time
                size_t nMax = m_nMaxFill.load(std::memory_order_relaxed);
                if(m_ring.FillFrames() > nMax)
                {
                    m_nTrimmed += m_ring.DropFrames(m_ring.FillFrames() - nMax);
                    m_nTrough = SIZE_MAX;
                    m_nWindow = 0;
                }
                else if(nCushion > 0)
                {
                    //give latency back once it is provably not needed: when the
                    //ring never dropped below cushion + 1 ms for a whole window,
                    //discard the surplus in one go (one click instead of a
                    //permanently longer path after a single stall)
                    m_nWindow += nFrames;
                    if(m_nWindow >= kTroughWindowFrames)
                    {
                        if(m_nTrough != SIZE_MAX && m_nTrough > nCushion + kTroughSlackFrames)
                        {
                            m_nTrimmed += m_ring.DropFrames(m_nTrough - nCushion);
                        }
                        m_nTrough = SIZE_MAX;
                        m_nWindow = 0;
                    }
                }
            }

            //--- sender side (audio::Source) ---
            bool Read(float* pInterleaved, size_t nFrames) override
            {
                const size_t nCount = m_vMap.size();
                size_t nGot = m_ring.PopFrames(pInterleaved, nFrames);
                if(nGot < nFrames)
                {
                    memset(pInterleaved + nGot*nCount, 0, (nFrames-nGot)*nCount*sizeof(float));
                    m_nUnderruns++;
                }
                return true;    //the device is live essence even when silent
            }

            std::string Describe() const override
            {
                std::string s = "AES67 Out ch";
                for(auto nCh : m_vMap) { s += " " + std::to_string(nCh+1); }
                return s;
            }
            size_t Channels() const override { return m_vMap.size(); }

            const std::vector<uint32_t>& Map() const { return m_vMap; }
            void SetMaxFill(size_t nFrames) { m_nMaxFill = nFrames; }
            /** frames the ring should still hold when the next chunk arrives.
            *   0 = off: the producer delivers evenly (gateway, self paced). **/
            void SetCushion(size_t nFrames) { m_nCushion = nFrames; }
            uint64_t Primed() const { return m_nPrimed.load(); }
            uint64_t Pushed() const { return m_nPushed.load(); }
            uint64_t Underruns() const { return m_nUnderruns.load(); }
            uint64_t Trimmed() const { return m_nTrimmed.load(); }
            size_t FillFrames() const { return m_ring.FillFrames(); }

        private:
            static std::vector<uint32_t> SanitizeMap(std::vector<uint32_t> vMap)
            {
                if(vMap.empty()) { vMap = {0, 1}; }
                if(vMap.size() > kMaxStreamChannels) { vMap.resize(kMaxStreamChannels); }
                for(auto& nCh : vMap) { nCh = std::min(nCh, kChannels-1); }
                return vMap;
            }

            const std::vector<uint32_t> m_vMap;     //stream ch i <- device ch map[i]
            SpscRing m_ring;
            std::vector<float> m_vSlice;    //io-proc scratch, no allocation in Push
            std::atomic<uint64_t> m_nUnderruns{0};
            std::atomic<uint64_t> m_nTrimmed{0};
            std::atomic<uint64_t> m_nPrimed{0};
            std::atomic<uint64_t> m_nPushed{0};     //frames taken from the producer
            std::atomic<size_t> m_nMaxFill{4*48 + 256};
            std::atomic<size_t> m_nCushion{0};
            //producer side only
            static constexpr size_t kTroughWindowFrames = 20 * 48000;
            static constexpr size_t kTroughSlackFrames = 48;
            size_t m_nTrough = SIZE_MAX;    //lowest fill seen right before a push
            size_t m_nWindow = 0;           //frames pushed in the current window
    };

    /** The live set of capture taps. The io proc walks an immutable snapshot
    *   that topology changes swap atomically - taps can come and go at runtime
    *   without locks on the audio path.
    **/
    class TxFanout
    {
        public:
            using Taps = std::vector<std::shared_ptr<TxTap>>;

            TxFanout() : m_pTaps(std::make_shared<const Taps>()) {}

            void SetTaps(Taps vTaps)
            {
                std::atomic_store(&m_pTaps, std::shared_ptr<const Taps>(
                    std::make_shared<const Taps>(std::move(vTaps))));
            }

            void SetCushion(size_t nFrames)
            {
                auto pTaps = std::atomic_load(&m_pTaps);
                for(const auto& pTap : *pTaps) { pTap->SetCushion(nFrames); }
            }

            //device channels any tap picks up (bit n = channel n)
            uint64_t ChannelMask() const
            {
                uint64_t nMask = 0;
                auto pTaps = std::atomic_load(&m_pTaps);
                for(const auto& pTap : *pTaps)
                {
                    for(auto nCh : pTap->Map()) { nMask |= 1ull << nCh; }
                }
                return nMask;
            }

            //io proc: distribute the device frames to every tap
            void Push(const float* pDeviceFrames, size_t nFrames)
            {
                auto pTaps = std::atomic_load(&m_pTaps);
                for(const auto& pTap : *pTaps) { pTap->Push(pDeviceFrames, nFrames); }
            }

        private:
            std::shared_ptr<const Taps> m_pTaps;
    };
}
