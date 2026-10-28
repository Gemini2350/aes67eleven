#pragma once
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
}
