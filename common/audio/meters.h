#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace pan::audio
{
    /** Lock-free stereo peak meter with decay, safe to feed from the audio
    *   thread and read from the web thread.
    **/
    class Meters
    {
        public:
            void Feed(const float* pInterleaved, size_t nFrames)
            {
                Feed(pInterleaved, nFrames, 2, 0, 1);
            }

            /** meter two picked channels out of an n-channel interleaved buffer **/
            void Feed(const float* pInterleaved, size_t nFrames, size_t nChannels, size_t nLeft, size_t nRight)
            {
                nLeft = std::min(nLeft, nChannels-1);
                nRight = std::min(nRight, nChannels-1);
                float fPeakL = 0.f, fPeakR = 0.f;
                for(size_t i = 0; i < nFrames; i++)
                {
                    fPeakL = std::max(fPeakL, std::fabs(pInterleaved[i*nChannels+nLeft]));
                    fPeakR = std::max(fPeakR, std::fabs(pInterleaved[i*nChannels+nRight]));
                }
                //ppm style: instant attack, ~20 dB per 1.5 s release - decay is
                //time based so the ballistics do not depend on the packet rate
                float fDecay = std::pow(10.0f, -13.3f * nFrames / 48000.0f / 20.0f);
                Update(m_fLeft, fPeakL, fDecay);
                Update(m_fRight, fPeakR, fDecay);
            }

            void Reset() { m_fLeft = 0.f; m_fRight = 0.f; }

            double LeftDb() const  { return ToDb(m_fLeft.load()); }
            double RightDb() const { return ToDb(m_fRight.load()); }

        private:
            static void Update(std::atomic<float>& fStore, float fPeak, float fDecay)
            {
                float fCurrent = fStore.load();
                float fDecayed = fCurrent * fDecay;
                fStore = std::max(fPeak, fDecayed);
            }
            static double ToDb(float f) { return f > 1e-6f ? 20.0*log10(f) : -120.0; }

            std::atomic<float> m_fLeft{0.f};
            std::atomic<float> m_fRight{0.f};
    };
}
