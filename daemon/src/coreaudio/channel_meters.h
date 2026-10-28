#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include "driver_iface.h"
#include "json.hpp"

namespace aes67vsc
{
    /** Lock-free per-channel peak meters for the full 64ch device, ppm-style
    *   ballistics (instant attack, ~20 dB per 1.5 s release). Fed from the io
    *   proc, read by the 20 Hz meter feed.
    **/
    class ChannelMeters
    {
        public:
            void Feed(const float* pInterleaved, size_t nFrames)
            {
                float fDecay = std::pow(10.0f, -13.3f * nFrames / 48000.0f / 20.0f);
                for(size_t nCh = 0; nCh < kChannels; nCh++)
                {
                    float fPeak = 0.f;
                    for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
                    {
                        fPeak = std::max(fPeak, std::fabs(pInterleaved[nFrame*kChannels + nCh]));
                    }
                    float fCurrent = m_afPeak[nCh].load(std::memory_order_relaxed) * fDecay;
                    m_afPeak[nCh].store(std::max(fPeak, fCurrent), std::memory_order_relaxed);
                }
            }

            nlohmann::json Db() const
            {
                nlohmann::json js = nlohmann::json::array();
                for(size_t nCh = 0; nCh < kChannels; nCh++)
                {
                    float f = m_afPeak[nCh].load(std::memory_order_relaxed);
                    js.push_back(f > 1e-6f ? std::round(20.0*std::log10(f)*10)/10.0 : -120.0);
                }
                return js;
            }

        private:
            std::array<std::atomic<float>, kChannels> m_afPeak{};
    };
}
