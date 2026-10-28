#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace aes67vsc
{
    /** Single producer / single consumer ring of interleaved float frames.
    *   Push from the io proc, pop from the rtp thread (or vice versa) - no
    *   locks, no allocation after construction.
    **/
    class SpscRing
    {
        public:
            SpscRing(size_t nFrames, size_t nChannels) :
                m_nCapacity(nFrames),
                m_nChannels(nChannels),
                m_vData(nFrames * nChannels, 0.f)
            {}

            size_t Channels() const { return m_nChannels; }
            size_t Capacity() const { return m_nCapacity; }

            size_t FillFrames() const
            {
                return m_nWritten.load(std::memory_order_acquire) - m_nRead.load(std::memory_order_acquire);
            }
            size_t FreeFrames() const { return m_nCapacity - FillFrames(); }

            /** returns frames actually pushed (rest dropped when full) **/
            size_t PushFrames(const float* pInterleaved, size_t nFrames)
            {
                size_t nPush = std::min(nFrames, FreeFrames());
                uint64_t nPos = m_nWritten.load(std::memory_order_relaxed);
                for(size_t nDone = 0; nDone < nPush;)
                {
                    size_t nIndex = size_t((nPos + nDone) % m_nCapacity);
                    size_t nChunk = std::min(nPush - nDone, m_nCapacity - nIndex);
                    memcpy(m_vData.data() + nIndex*m_nChannels,
                           pInterleaved + nDone*m_nChannels,
                           nChunk * m_nChannels * sizeof(float));
                    nDone += nChunk;
                }
                m_nWritten.store(nPos + nPush, std::memory_order_release);
                return nPush;
            }

            /** returns frames actually popped (caller zero-fills the rest) **/
            size_t PopFrames(float* pInterleaved, size_t nFrames)
            {
                size_t nPop = std::min(nFrames, FillFrames());
                uint64_t nPos = m_nRead.load(std::memory_order_relaxed);
                for(size_t nDone = 0; nDone < nPop;)
                {
                    size_t nIndex = size_t((nPos + nDone) % m_nCapacity);
                    size_t nChunk = std::min(nPop - nDone, m_nCapacity - nIndex);
                    memcpy(pInterleaved + nDone*m_nChannels,
                           m_vData.data() + nIndex*m_nChannels,
                           nChunk * m_nChannels * sizeof(float));
                    nDone += nChunk;
                }
                m_nRead.store(nPos + nPop, std::memory_order_release);
                return nPop;
            }

            /** consumer-side discard, e.g. to bound latency **/
            size_t DropFrames(size_t nFrames)
            {
                size_t nDrop = std::min(nFrames, FillFrames());
                m_nRead.fetch_add(nDrop, std::memory_order_release);
                return nDrop;
            }

            void Clear()
            {
                m_nRead.store(m_nWritten.load(std::memory_order_acquire), std::memory_order_release);
            }

        private:
            const size_t m_nCapacity;
            const size_t m_nChannels;
            std::vector<float> m_vData;
            std::atomic<uint64_t> m_nWritten{0};
            std::atomic<uint64_t> m_nRead{0};
    };
}
