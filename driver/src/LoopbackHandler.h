#pragma once
#include <aspl/IORequestHandler.hpp>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "driver_iface.h"

namespace aes67vsc
{
    /** Timeline-addressed sample ring: slots addressed by absolute sample time.
    *   Works across the two cross-wired devices because both share one zero
    *   timestamp generator (identical sample clocks).
    *
    *   Realtime rules: no locks, no allocation, no logging. The only shared
    *   state is the sample ring plus one atomic high-water mark.
    **/
    class TimelineRing
    {
        public:
            TimelineRing() : m_vRing(kRingFrames * kChannels, 0.f) {}

            void Write(int64_t nStart, const float* pSrc, int64_t nFrames)
            {
                //a paused/restarted writer leaves a gap - silence it so stale
                //ring content does not replay
                int64_t nLastEnd = m_nWrittenEnd.load(std::memory_order_relaxed);
                if(nLastEnd > 0 && nStart > nLastEnd)
                {
                    for(int64_t nFrame = std::max(nLastEnd, nStart - int64_t(kRingFrames)); nFrame < nStart; nFrame++)
                    {
                        memset(RingAt(nFrame), 0, kChannels * sizeof(float));
                    }
                }
                for(int64_t nFrame = 0; nFrame < nFrames; nFrame++)
                {
                    memcpy(RingAt(nStart + nFrame), pSrc + nFrame*kChannels, kChannels * sizeof(float));
                }
                m_nWrittenEnd.store(nStart + nFrames, std::memory_order_release);
            }

            int64_t WrittenEnd() const { return m_nWrittenEnd.load(std::memory_order_relaxed); }

            int64_t LastRead() const { return m_nLastRead.load(std::memory_order_relaxed); }

            void Read(int64_t nStart, float* pDst, int64_t nFrames)
            {
                m_nLastRead.store(nStart, std::memory_order_relaxed);
                int64_t nEnd = m_nWrittenEnd.load(std::memory_order_acquire);
                for(int64_t nFrame = 0; nFrame < nFrames; nFrame++)
                {
                    int64_t nAbs = nStart + nFrame;
                    float* pOut = pDst + nFrame*kChannels;
                    //silence outside the written window (nothing playing, or too old)
                    if(nAbs >= nEnd || nAbs < nEnd - int64_t(kRingFrames))
                    {
                        memset(pOut, 0, kChannels * sizeof(float));
                    }
                    else
                    {
                        memcpy(pOut, RingAt(nAbs), kChannels * sizeof(float));
                    }
                }
            }

        private:
            static constexpr size_t kRingFrames = 16384;    //~341 ms at 48k

            float* RingAt(int64_t nAbsFrame)
            {
                return m_vRing.data() + size_t(uint64_t(nAbsFrame) % kRingFrames) * kChannels;
            }

            std::vector<float> m_vRing;
            std::atomic<int64_t> m_nWrittenEnd{0};
            std::atomic<int64_t> m_nLastRead{0};
    };

    /** SPSC fifo of interleaved 64ch frames the daemon drains through the
    *   capture tap property (producer: coreaudiod io thread, consumer: hal
    *   property getter on a non-realtime thread). Lock free. **/
    class CaptureFifo
    {
        public:
            CaptureFifo() : m_vData(size_t(kCaptureRingFrames) * kChannels, 0.f) {}

            void Push(const float* pInterleaved, size_t nFrames)
            {
                uint64_t nWrite = m_nWritten.load(std::memory_order_relaxed);
                uint64_t nRead = m_nRead.load(std::memory_order_acquire);
                size_t nFree = kCaptureRingFrames - size_t(nWrite - nRead);
                if(nFrames > nFree)
                {
                    //consumer stalled - drop the oldest by advancing read
                    m_nRead.store(nWrite + nFrames - kCaptureRingFrames, std::memory_order_release);
                    m_nOverruns.fetch_add(1, std::memory_order_relaxed);
                }
                for(size_t nDone = 0; nDone < nFrames;)
                {
                    size_t nIndex = size_t((nWrite + nDone) % kCaptureRingFrames);
                    size_t nChunk = std::min(nFrames - nDone, size_t(kCaptureRingFrames) - nIndex);
                    memcpy(m_vData.data() + nIndex*kChannels, pInterleaved + nDone*kChannels,
                           nChunk * kChannels * sizeof(float));
                    nDone += nChunk;
                }
                m_nWritten.store(nWrite + nFrames, std::memory_order_release);
            }

            size_t Pop(float* pOut, size_t nMaxFrames)
            {
                uint64_t nRead = m_nRead.load(std::memory_order_relaxed);
                uint64_t nWrite = m_nWritten.load(std::memory_order_acquire);
                size_t nAvail = size_t(nWrite - nRead);
                size_t nPop = std::min(nAvail, nMaxFrames);
                for(size_t nDone = 0; nDone < nPop;)
                {
                    size_t nIndex = size_t((nRead + nDone) % kCaptureRingFrames);
                    size_t nChunk = std::min(nPop - nDone, size_t(kCaptureRingFrames) - nIndex);
                    memcpy(pOut + nDone*kChannels, m_vData.data() + nIndex*kChannels,
                           nChunk * kChannels * sizeof(float));
                    nDone += nChunk;
                }
                m_nRead.store(nRead + nPop, std::memory_order_release);
                return nPop;
            }

            uint64_t Overruns() const { return m_nOverruns.load(std::memory_order_relaxed); }
            uint64_t Written() const { return m_nWritten.load(std::memory_order_relaxed); }
            uint64_t Read() const { return m_nRead.load(std::memory_order_relaxed); }

        private:
            std::vector<float> m_vData;
            std::atomic<uint64_t> m_nWritten{0};
            std::atomic<uint64_t> m_nRead{0};
            std::atomic<uint64_t> m_nOverruns{0};
    };

    /** Writer side of the shared capture ring (see driver_iface.h). Mapped
    *   once when the driver loads; where the sandbox refuses both variants
    *   the ring stays unmapped and Write() is a no-op. **/
    class CaptureShm
    {
        public:
            CaptureShm()
            {
                int nFd = shm_open(kCaptureShmName, O_RDWR | O_CREAT, 0644);
                if(nFd >= 0 && !Map(nFd, true))
                {
                    //an object of another size (older layout) cannot be resized
                    m_nShmErrno = errno;
                    shm_unlink(kCaptureShmName);
                    nFd = shm_open(kCaptureShmName, O_RDWR | O_CREAT | O_EXCL, 0644);
                    if(nFd >= 0 && Map(nFd, true)) { m_nShmErrno = 0; }
                }
                else if(nFd < 0)
                {
                    m_nShmErrno = errno;
                }
                if(m_pHeader) { m_sKind = "shm"; return; }

                nFd = open(kCaptureShmFile, O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
                if(nFd < 0 || !Map(nFd, false)) { m_nFileErrno = errno; return; }
                m_sKind = "file";
            }

            //io thread: no locks, no allocation, the pages are prefaulted
            void Write(const float* pInterleaved, size_t nFrames)
            {
                if(!m_pHeader) { return; }
                uint64_t nWrite = m_pHeader->nWritten.load(std::memory_order_relaxed);
                for(size_t nDone = 0; nDone < nFrames;)
                {
                    size_t nIndex = size_t((nWrite + nDone) % kCaptureShmFrames);
                    size_t nChunk = std::min(nFrames - nDone, size_t(kCaptureShmFrames) - nIndex);
                    memcpy(m_pData + nIndex*kChannels, pInterleaved + nDone*kChannels,
                           nChunk * kChannels * sizeof(float));
                    nDone += nChunk;
                }
                m_pHeader->nWritten.store(nWrite + nFrames, std::memory_order_release);
            }

            const char* Kind() const { return m_sKind; }
            int ShmErrno() const { return m_nShmErrno; }
            int FileErrno() const { return m_nFileErrno; }

        private:
            bool Map(int nFd, bool bShm)
            {
                struct stat st{};
                if(fstat(nFd, &st) != 0) { close(nFd); return false; }
                if(size_t(st.st_size) != kCaptureShmBytes)
                {
                    //posix shm on macos can be sized exactly once
                    if((bShm && st.st_size != 0) || ftruncate(nFd, off_t(kCaptureShmBytes)) != 0)
                    {
                        int nError = errno ? errno : EINVAL;
                        close(nFd);
                        errno = nError;
                        return false;
                    }
                }
                fchmod(nFd, 0644);      //the daemon runs as the user and only reads
                void* p = mmap(nullptr, kCaptureShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, 0);
                int nError = errno;
                close(nFd);
                if(p == MAP_FAILED) { errno = nError; return false; }
                memset(p, 0, kCaptureShmBytes);
                mlock(p, kCaptureShmBytes);
                auto* pHeader = static_cast<CaptureShmHeader*>(p);
                pHeader->nChannels = kChannels;
                pHeader->nFrames = kCaptureShmFrames;
                pHeader->nWritten.store(0);
                pHeader->nEpoch.store(mach_absolute_time());
                pHeader->nMagic = kCaptureShmMagic;
                m_pData = reinterpret_cast<float*>(static_cast<uint8_t*>(p) + kCaptureShmDataOffset);
                m_pHeader = pHeader;
                return true;
            }

            CaptureShmHeader* m_pHeader = nullptr;
            float* m_pData = nullptr;
            const char* m_sKind = "none";
            int m_nShmErrno = 0;
            int m_nFileErrno = 0;
    };

    /** the two cross-wired paths between the visible device and the bridge **/
    struct SharedRings
    {
        TimelineRing txRing;    //apps play on "AES67" -> daemon captures on bridge
        TimelineRing rxRing;    //daemon renders on bridge -> apps record on "AES67"
        CaptureFifo txCapture;  //apps play on "AES67" -> daemon drains via property (no mic io)
        std::atomic<uint64_t> nCaptureMask{~0ull};  //channels the packed tap delivers
        CaptureShm txShm;       //the same audio, mapped by the daemon (fast path)
        //tap property getters: scratch buffers, one reader at a time
        std::mutex tapMutex;
        std::vector<float> vTapChunk = std::vector<float>(size_t(kCaptureChunkFrames) * kChannels);
        std::vector<uint8_t> vTapOut;
        std::atomic<uint64_t> nTapCalls{0}, nTapFrames{0};
        //diagnostics: io dispatch counters per role
        std::atomic<uint64_t> nMainReads{0}, nMainWrites{0}, nBridgeReads{0}, nBridgeWrites{0};
    };

    /** io handler for either device; bIsBridge picks the wiring direction **/
    class CrossWireHandler : public aspl::IORequestHandler
    {
        public:
            CrossWireHandler(std::shared_ptr<SharedRings> pRings, bool bIsBridge) :
                m_pRings(std::move(pRings)),
                m_bIsBridge(bIsBridge)
            {}

            void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                                    Float64 zeroTimestamp, Float64 timestamp,
                                    const void* pBytes, UInt32 nBytesCount) override
            {
                (m_bIsBridge ? m_pRings->nBridgeWrites : m_pRings->nMainWrites)++;
                auto nFrames = static_cast<int64_t>(nBytesCount / (kChannels * sizeof(float)));
                if(nFrames <= 0) { return; }
                auto& ring = m_bIsBridge ? m_pRings->rxRing : m_pRings->txRing;
                ring.Write(static_cast<int64_t>(timestamp), static_cast<const float*>(pBytes), nFrames);
                if(!m_bIsBridge)
                {
                    m_pRings->txCapture.Push(static_cast<const float*>(pBytes), size_t(nFrames));
                    m_pRings->txShm.Write(static_cast<const float*>(pBytes), size_t(nFrames));
                }
            }

            void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                                   const std::shared_ptr<aspl::Stream>& stream,
                                   Float64 zeroTimestamp, Float64 timestamp,
                                   void* pBytes, UInt32 nBytesCount) override
            {
                (m_bIsBridge ? m_pRings->nBridgeReads : m_pRings->nMainReads)++;
                auto nFrames = static_cast<int64_t>(nBytesCount / (kChannels * sizeof(float)));
                if(nFrames <= 0) { return; }
                auto& ring = m_bIsBridge ? m_pRings->txRing : m_pRings->rxRing;
                ring.Read(static_cast<int64_t>(timestamp), static_cast<float*>(pBytes), nFrames);
            }

        private:
            std::shared_ptr<SharedRings> m_pRings;
            const bool m_bIsBridge;
    };
}
