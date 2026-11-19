#include "PtpClockedDevice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mach/mach_time.h>

using namespace aes67vsc;

PtpClockedDevice::PtpClockedDevice(const std::shared_ptr<const aspl::Context>& context,
                                   const aspl::DeviceParameters& params,
                                   std::shared_ptr<DriverClock> pClock,
                                   std::shared_ptr<SharedRings> pRings,
                                   UInt32 nTransportType) :
    aspl::Device(context, params),
    m_pClock(std::move(pClock)),
    m_pRings(std::move(pRings)),
    m_nTransportType(nTransportType)
{
    std::lock_guard<std::mutex> lg(m_pClock->mutex);
    if(m_pClock->dHostTicksPerSec == 0.0)
    {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        m_pClock->dHostTicksPerSec = 1e9 * double(timebase.denom) / double(timebase.numer);
    }

    RegisterCustomProperty(kClockSyncSelector, *this,
                           &PtpClockedDevice::GetClockSyncProperty,
                           &PtpClockedDevice::SetClockSyncProperty);
    RegisterCustomProperty(kCaptureTapSelector, *this, &PtpClockedDevice::GetCaptureTap);
    RegisterCustomProperty(kCapturePackedSelector, *this, &PtpClockedDevice::GetCapturePacked);
    RegisterCustomProperty(kCaptureDiagSelector, *this, &PtpClockedDevice::GetCaptureDiag);
    RegisterCustomProperty(kCaptureMaskSelector, *this,
                           &PtpClockedDevice::GetCaptureMask,
                           &PtpClockedDevice::SetCaptureMask);
}

CFPropertyListRef PtpClockedDevice::GetCapturePacked() const
{
    if(!m_pRings) { return CFDataCreate(kCFAllocatorDefault, nullptr, 0); }
    std::lock_guard<std::mutex> lg(m_pRings->tapMutex);
    auto& vChunk = m_pRings->vTapChunk;
    auto& vOut = m_pRings->vTapOut;
    size_t nFrames = m_pRings->txCapture.Pop(vChunk.data(), kCaptureChunkFrames);
    m_pRings->nTapCalls++;
    m_pRings->nTapFrames += nFrames;
    if(nFrames == 0)
    {
        return CFDataCreate(kCFAllocatorDefault, nullptr, 0);
    }

    CapturePackedHeader header{};
    header.nMagic = kCapturePackedMagic;
    header.nFrames = uint32_t(nFrames);
    const uint64_t nMask = m_pRings->nCaptureMask.load(std::memory_order_relaxed);
    for(uint32_t nCh = 0; nCh < kChannels; nCh++)
    {
        if(nMask & (1ull << nCh)) { header.aChannel[header.nChannels++] = uint8_t(nCh); }
        float fPeak = 0.f;
        for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
        {
            fPeak = std::max(fPeak, std::fabs(vChunk[nFrame*kChannels + nCh]));
        }
        header.afPeak[nCh] = fPeak;
    }

    vOut.resize(sizeof(header) + nFrames * header.nChannels * sizeof(float));
    memcpy(vOut.data(), &header, sizeof(header));
    auto* pDst = reinterpret_cast<float*>(vOut.data() + sizeof(header));
    for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
    {
        const float* pSrc = vChunk.data() + nFrame*kChannels;
        for(uint32_t n = 0; n < header.nChannels; n++) { *pDst++ = pSrc[header.aChannel[n]]; }
    }
    return CFDataCreate(kCFAllocatorDefault, vOut.data(), CFIndex(vOut.size()));
}

CFStringRef PtpClockedDevice::GetCaptureDiag() const
{
    char buffer[256] = "kind=none";
    if(m_pRings)
    {
        snprintf(buffer, sizeof(buffer),
                 "kind=%s shm_errno=%d file_errno=%d fifo_written=%llu fifo_read=%llu fifo_overruns=%llu tap_calls=%llu tap_frames=%llu",
                 m_pRings->txShm.Kind(), m_pRings->txShm.ShmErrno(), m_pRings->txShm.FileErrno(),
                 (unsigned long long)m_pRings->txCapture.Written(),
                 (unsigned long long)m_pRings->txCapture.Read(),
                 (unsigned long long)m_pRings->txCapture.Overruns(),
                 (unsigned long long)m_pRings->nTapCalls.load(),
                 (unsigned long long)m_pRings->nTapFrames.load());
    }
    return CFStringCreateWithCString(kCFAllocatorDefault, buffer, kCFStringEncodingUTF8);
}

CFStringRef PtpClockedDevice::GetCaptureMask() const
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llx",
             static_cast<unsigned long long>(m_pRings ? m_pRings->nCaptureMask.load() : 0));
    return CFStringCreateWithCString(kCFAllocatorDefault, buffer, kCFStringEncodingUTF8);
}

void PtpClockedDevice::SetCaptureMask(CFStringRef value)
{
    if(!value || !m_pRings) { return; }
    char buffer[32] = {0};
    if(!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) { return; }
    unsigned long long nMask = 0;
    if(sscanf(buffer, "%llx", &nMask) != 1) { return; }
    m_pRings->nCaptureMask.store(nMask);
}

CFPropertyListRef PtpClockedDevice::GetCaptureTap() const
{
    //drain up to one chunk of what apps played on the main device. returned
    //as CFData of interleaved float frames; empty data = nothing new.
    if(!m_pRings) { return CFDataCreate(kCFAllocatorDefault, nullptr, 0); }
    std::lock_guard<std::mutex> lg(m_pRings->tapMutex);
    auto& vChunk = m_pRings->vTapChunk;
    size_t nFrames = m_pRings->txCapture.Pop(vChunk.data(), kCaptureChunkFrames);
    m_pRings->nTapCalls++;
    m_pRings->nTapFrames += nFrames;
    return CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(vChunk.data()),
                        CFIndex(nFrames * kChannels * sizeof(float)));
}

OSStatus PtpClockedDevice::GetZeroTimeStampImpl(UInt32 nClientID,
                                                Float64* pOutSampleTime,
                                                UInt64* pOutHostTime,
                                                UInt64* pOutSeed)
{
    const uint64_t nNow = mach_absolute_time();
    const double dPeriodFrames = double(GetZeroTimeStampPeriod());

    //the timeline is SHARED between the visible device and the bridge - both
    //return identical (sample time, host time) pairs, which is what keeps the
    //cross-wired rings aligned. the lock spans a handful of arithmetic ops.
    auto& clock = *m_pClock;
    std::lock_guard<std::mutex> lg(clock.mutex);

    if(clock.nAnchorHostTicks == 0)
    {
        clock.nAnchorHostTicks = nNow;
        clock.nPeriodHostTicks = nNow;
        clock.nPeriodCounter = 0;
        clock.dRate = 1.0 + double(clock.nTargetRatePpb.load(std::memory_order_relaxed)) / 1e9;
    }

    //advance whole periods; each period the rate slews a little towards the
    //daemon's target, so ptp steps never bend the timeline abruptly. sample
    //time stays exact multiples of the period, host time absorbs the rate.
    for(;;)
    {
        double dTarget = 1.0 + double(clock.nTargetRatePpb.load(std::memory_order_relaxed)) / 1e9;
        dTarget = std::clamp(dTarget, 1.0 - 500e-6, 1.0 + 500e-6);
        //<= ~2 ppm change per ~21ms period: inaudible, converges within seconds
        clock.dRate += std::clamp(dTarget - clock.dRate, -2e-6, 2e-6);

        double dTicksPerFrame = clock.dHostTicksPerSec / (double(kSampleRate) * clock.dRate);
        auto nTicksPerPeriod = uint64_t(dTicksPerFrame * dPeriodFrames);
        if(nNow < clock.nPeriodHostTicks + nTicksPerPeriod) { break; }
        clock.nPeriodHostTicks += nTicksPerPeriod;
        clock.nPeriodCounter++;
    }

    *pOutSampleTime = double(clock.nPeriodCounter) * dPeriodFrames;
    *pOutHostTime = clock.nPeriodHostTicks;
    *pOutSeed = 1;
    return kAudioHardwareNoError;
}

CFStringRef PtpClockedDevice::GetClockSyncProperty() const
{
    //"ppb locked updates txEnd txRead rxEnd rxRead sample" - the ring
    //positions expose exactly where the cross-wire stands
    long long nSample = 0;
    {
        std::lock_guard<std::mutex> lg(m_pClock->mutex);
        nSample = (long long)(m_pClock->nPeriodCounter) * 1024;
    }
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%lld %d %llu %lld %lld %lld %lld %lld %llu %llu %llu %llu",
             static_cast<long long>(m_pClock->nTargetRatePpb.load()),
             m_pClock->bLocked.load() ? 1 : 0,
             static_cast<unsigned long long>(m_pClock->nUpdates.load()),
             static_cast<long long>(m_pRings ? m_pRings->txRing.WrittenEnd() : -1),
             static_cast<long long>(m_pRings ? m_pRings->txRing.LastRead() : -1),
             static_cast<long long>(m_pRings ? m_pRings->rxRing.WrittenEnd() : -1),
             static_cast<long long>(m_pRings ? m_pRings->rxRing.LastRead() : -1),
             nSample,
             (unsigned long long)(m_pRings ? m_pRings->nMainReads.load() : 0),
             (unsigned long long)(m_pRings ? m_pRings->nMainWrites.load() : 0),
             (unsigned long long)(m_pRings ? m_pRings->nBridgeReads.load() : 0),
             (unsigned long long)(m_pRings ? m_pRings->nBridgeWrites.load() : 0));
    return CFStringCreateWithCString(kCFAllocatorDefault, buffer, kCFStringEncodingUTF8);
}

void PtpClockedDevice::SetClockSyncProperty(CFStringRef value)
{
    if(!value) { return; }
    char buffer[96] = {0};
    if(!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) { return; }

    long long nRatePpb = 0;
    int nLocked = 0;
    if(sscanf(buffer, "%lld %d", &nRatePpb, &nLocked) < 1) { return; }

    //holdover on unlock: keep the last rate rather than snapping back to nominal
    if(nLocked)
    {
        m_pClock->nTargetRatePpb.store(std::clamp<long long>(nRatePpb, -500'000, 500'000));
    }
    m_pClock->bLocked.store(nLocked != 0);
    m_pClock->nUpdates.fetch_add(1);
}
