#pragma once
#include <cstddef>
#include <cstdint>

namespace pan::audio
{
    /** Playout sink fed by the rtp receiver: interleaved float frames in the
    *   stream's channel count. Write() blocks until the frames are queued (that
    *   backpressure paces the playout loop); DelayFrames() reports how much
    *   audio is buffered ahead of the consumer so playout can steer.
    **/
    class Sink
    {
        public:
            virtual ~Sink() = default;
            virtual bool Write(const float* pInterleaved, size_t nFrames, size_t nChannels, double dGainDb) = 0;
            virtual long DelayFrames() = 0;
            virtual uint64_t Underruns() const = 0;
    };
}
