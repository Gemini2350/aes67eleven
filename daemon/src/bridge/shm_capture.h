#pragma once
#include <cstring>
#include <vector>
#include "driver_iface.h"
#ifdef __APPLE__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aes67vsc
{
    /** Reader side of the driver's shared capture ring (see driver_iface.h):
    *   the audio apps play on "AES67eleven", all 64 channels, read straight
    *   from memory the driver's io thread writes - no ipc per block. Lock
    *   free; a reader that falls behind (or a driver that started over)
    *   resyncs to the write position instead of delivering stale audio.
    **/
    class ShmCapture
    {
        public:
            ~ShmCapture() { Close(); }

#ifdef __APPLE__
            bool Open()
            {
                Close();
                int nFd = shm_open(kCaptureShmName, O_RDONLY, 0);
                if(nFd < 0) { nFd = open(kCaptureShmFile, O_RDONLY | O_NOFOLLOW); }
                if(nFd < 0) { return false; }
                struct stat st{};
                void* p = MAP_FAILED;
                if(fstat(nFd, &st) == 0 && size_t(st.st_size) >= kCaptureShmBytes)
                {
                    p = mmap(nullptr, kCaptureShmBytes, PROT_READ, MAP_SHARED, nFd, 0);
                }
                close(nFd);
                if(p == MAP_FAILED) { return false; }
                auto* pHeader = static_cast<const CaptureShmHeader*>(p);
                if(pHeader->nMagic != kCaptureShmMagic || pHeader->nChannels != kChannels ||
                   pHeader->nFrames != kCaptureShmFrames)
                {
                    munmap(p, kCaptureShmBytes);
                    return false;
                }
                m_pHeader = pHeader;
                m_pData = reinterpret_cast<const float*>(static_cast<const uint8_t*>(p) + kCaptureShmDataOffset);
                m_nEpoch = pHeader->nEpoch.load(std::memory_order_acquire);
                m_nRead = pHeader->nWritten.load(std::memory_order_acquire);
                return true;
            }

            void Close()
            {
                if(m_pHeader) { munmap(const_cast<CaptureShmHeader*>(m_pHeader), kCaptureShmBytes); }
                m_pHeader = nullptr;
                m_pData = nullptr;
            }
#else
            bool Open() { return false; }
            void Close() {}
#endif

            bool IsOpen() const { return m_pHeader != nullptr; }
            uint64_t Written() const { return m_pHeader ? m_pHeader->nWritten.load(std::memory_order_acquire) : 0; }
            uint64_t Resyncs() const { return m_nResyncs; }

            /** appends nothing, replaces vOut with the new frames (interleaved
            *   64ch); returns the frame count, 0 = nothing new **/
            size_t Read(std::vector<float>& vOut, size_t nMaxFrames = 2048)
            {
                if(!m_pHeader) { return 0; }
                uint64_t nEpoch = m_pHeader->nEpoch.load(std::memory_order_acquire);
                uint64_t nWritten = m_pHeader->nWritten.load(std::memory_order_acquire);
                if(nEpoch != m_nEpoch || nWritten < m_nRead || nWritten - m_nRead > kCaptureShmFrames/2)
                {
                    m_nEpoch = nEpoch;
                    m_nRead = nWritten;
                    m_nResyncs++;
                    return 0;
                }
                size_t nFrames = std::min(size_t(nWritten - m_nRead), nMaxFrames);
                if(nFrames == 0) { return 0; }
                vOut.resize(nFrames * kChannels);
                for(size_t nDone = 0; nDone < nFrames;)
                {
                    size_t nIndex = size_t((m_nRead + nDone) % kCaptureShmFrames);
                    size_t nChunk = std::min(nFrames - nDone, size_t(kCaptureShmFrames) - nIndex);
                    memcpy(vOut.data() + nDone*kChannels, m_pData + nIndex*kChannels,
                           nChunk * kChannels * sizeof(float));
                    nDone += nChunk;
                }
                //the writer lapped us while we copied? then this block is torn
                if(m_pHeader->nWritten.load(std::memory_order_acquire) - m_nRead > kCaptureShmFrames)
                {
                    m_nRead = m_pHeader->nWritten.load(std::memory_order_acquire);
                    m_nResyncs++;
                    return 0;
                }
                m_nRead += nFrames;
                return nFrames;
            }

        private:
            const CaptureShmHeader* m_pHeader = nullptr;
            const float* m_pData = nullptr;
            uint64_t m_nEpoch = 0;
            uint64_t m_nRead = 0;
            uint64_t m_nResyncs = 0;
    };
}
