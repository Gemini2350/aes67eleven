#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

/** Shared contract between the HAL driver and the daemon. **/
namespace aes67vsc
{
    //one visible device "AES67" for apps (output -> network tx, input <- network
    //rx) plus a hidden bridge device the daemon drives: the driver cross-wires
    //them (app output -> bridge input, bridge output -> app input) on a shared
    //timeline, so apps see a single ordinary 64x64 soundcard
    constexpr const char* kDeviceUidMain = "AES67VSC:Device";
    constexpr const char* kDeviceUidBridge = "AES67VSC:Bridge";

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 64;
    //aes67 @48k/1ms/L24 caps a single stream at 8 channels (mtu), so the 64
    //device channels are served by up to 8 senders + 8 receivers on channel
    //ranges (offset + count), like a dante virtual soundcard
    constexpr uint32_t kMaxStreamChannels = 8;
    constexpr uint32_t kMaxStreams = 64;    //up to 64 mono streams onto the matrix

    //both devices share one clock domain - they are rate-locked to the same
    //ptp discipline, so aggregates must not resample between them
    constexpr uint32_t kClockDomain = 0x50545043;   //'PTPC'

    /** custom property (CFString) on both devices, set by the daemon ~1x/s:
    *       "<rate_ppb> <locked>"
    *   rate_ppb = (d(ptp)/d(host) - 1) * 1e9, locked = 0/1.
    *   The driver slews its zero-timestamp rate towards it; without updates
    *   (or locked=0) it holds the last rate (holdover).
    *   The getter returns "<rate_ppb> <locked> <updates>" for diagnostics. **/
    constexpr uint32_t kClockSyncSelector = 0x70747053;     //'ptpS'

    /** capture tap (CFData) on the main device: returns the audio apps played
    *   onto "AES67eleven" since the last read - up to kCaptureChunkFrames
    *   interleaved 64ch float frames per call, drained by the daemon at
    *   ~10-20 ms cadence. This replaces the input io proc on the bridge, so
    *   the daemon never captures "microphone" audio (no orange indicator). **/
    constexpr uint32_t kCaptureTapSelector = 0x63617054;    //'capT'
    constexpr uint32_t kCaptureChunkFrames = 4096;
    constexpr uint32_t kCaptureRingFrames = 65536;          //~1.4 s at 48k

    /** packed capture tap (CFData) on the main device: the same frames as the
    *   capture tap, but only the channels the daemon asked for. A read with
    *   all 64 channels carries ~100 kB and takes several ms through
    *   coreaudiod; two channels take a fraction of a ms, so the daemon can
    *   drain every 2 ms and keep the path short. Layout: CapturePackedHeader,
    *   then nFrames * nChannels interleaved floats. Empty data = nothing new.
    *   The header carries the peak of all 64 device channels, so the meters
    *   also show channels no sender picks up. **/
    constexpr uint32_t kCapturePackedSelector = 0x63617050; //'capP'
    constexpr uint32_t kCapturePackedMagic = 0x31504143;    //"CAP1"
    struct CapturePackedHeader
    {
        uint32_t nMagic;
        uint32_t nFrames;
        uint32_t nChannels;             //channels per packed frame
        uint8_t aChannel[kChannels];    //device channel of packed channel i
        float afPeak[kChannels];        //peak per device channel over nFrames
    };

    /** channel selection for the packed tap (CFString, hex bit mask, bit n =
    *   device channel n), set by the daemon whenever its senders change **/
    constexpr uint32_t kCaptureMaskSelector = 0x6361704D;   //'capM'

    /** shared capture ring: the driver's io thread writes what apps play on
    *   "AES67eleven" straight into memory the daemon maps read-only - all 64
    *   channels, no ipc per block. This is the fast path; the tap properties
    *   above stay as fallback where the driver's sandbox refuses the mapping.
    *   Layout: CaptureShmHeader, audio from kCaptureShmDataOffset on as
    *   kCaptureShmFrames * kChannels interleaved floats. Frame n lives in
    *   slot n % kCaptureShmFrames; nWritten is the number of frames written
    *   since nEpoch (which changes whenever the driver starts over). **/
    constexpr const char* kCaptureShmName = "/aes67eleven.tx1";             //posix shm
    constexpr const char* kCaptureShmFile = "/tmp/aes67eleven-tx1.ring";    //mapped file, 2nd choice
    constexpr uint32_t kCaptureShmMagic = 0x31524854;       //"THR1"
    constexpr uint32_t kCaptureShmFrames = 16384;           //~341 ms at 48k
    constexpr size_t kCaptureShmDataOffset = 16384;
    constexpr size_t kCaptureShmBytes = kCaptureShmDataOffset + size_t(kCaptureShmFrames) * kChannels * sizeof(float);
    struct CaptureShmHeader
    {
        uint32_t nMagic;
        uint32_t nChannels;
        uint32_t nFrames;
        uint32_t nReserved;
        std::atomic<uint64_t> nWritten;
        std::atomic<uint64_t> nEpoch;
    };

    /** capture diagnostics (CFString) on the main device, e.g.
    *   "kind=shm shm_errno=0 file_errno=0 fifo_written=.. fifo_read=.. ..."
    *   kind = shm | file | none tells the daemon which path the driver offers **/
    constexpr uint32_t kCaptureDiagSelector = 0x63617053;   //'capS'
}
