#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "driver_iface.h"
#include "json.hpp"
#include "audio/sources.h"

namespace aes67vsc
{
    /** one web-ui file player: a wav/mp3 decoded to stereo 48k, mixed onto a
    *   device channel pair of the 64ch tx bus. Mix() runs on the capture
    *   drain thread; control calls come from the web thread - the mutex is
    *   uncontended outside of clicks, so the drain path stays cheap.
    **/
    class PlayerDeck
    {
        public:
            bool Load(const std::string& sPath, const std::string& sName)
            {
                auto vSamples = pan::audio::LoadFileStereo48k(sPath);
                std::lock_guard<std::mutex> lg(m_mutex);
                if(vSamples.empty()) { return false; }
                m_pSamples = std::make_shared<std::vector<float>>(std::move(vSamples));
                m_sFile = sName;
                m_nPos = 0;
                //keep playing across a file switch - the deck restarts at 0
                //with the new material instead of silently stopping
                return true;
            }

            void Control(const std::string& sCommand)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                if(sCommand == "play" && m_pSamples)
                {
                    if(m_nPos >= m_pSamples->size()/2) { m_nPos = 0; }
                    m_bPlaying = true;
                }
                else if(sCommand == "pause") { m_bPlaying = false; }
                else if(sCommand == "stop")  { m_bPlaying = false; m_nPos = 0; }
            }

            void Seek(double dSeconds)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                if(!m_pSamples) { return; }
                auto nFrame = static_cast<size_t>(std::max(0.0, dSeconds) * kSampleRate);
                m_nPos = std::min(nFrame, m_pSamples->size()/2);
            }

            void Settings(const nlohmann::json& jsBody)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                if(jsBody.contains("loop"))    { m_bLoop = jsBody["loop"].get<bool>(); }
                if(jsBody.contains("autostart")) { m_bAutostart = jsBody["autostart"].get<bool>(); }
                if(jsBody.contains("gain_db")) { m_dGainDb = jsBody["gain_db"].get<double>(); }
                if(jsBody.contains("ch_left"))
                {
                    m_nChLeft = std::clamp(jsBody["ch_left"].get<int>(), 0, int(kChannels)-1);
                }
                if(jsBody.contains("ch_right"))
                {
                    m_nChRight = std::clamp(jsBody["ch_right"].get<int>(), 0, int(kChannels)-1);
                }
            }

            bool Playing() const
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                return m_bPlaying;
            }

            nlohmann::json Status() const
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                size_t nTotal = m_pSamples ? m_pSamples->size()/2 : 0;
                return {{"file", m_sFile}, {"playing", m_bPlaying}, {"loop", m_bLoop},
                        {"autostart", m_bAutostart},
                        {"gain_db", m_dGainDb}, {"ch_left", m_nChLeft}, {"ch_right", m_nChRight},
                        {"position_s", double(m_nPos) / kSampleRate},
                        {"duration_s", double(nTotal) / kSampleRate}};
            }

            nlohmann::json Persist() const
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                return {{"file", m_sFile}, {"loop", m_bLoop}, {"autostart", m_bAutostart},
                        {"gain_db", m_dGainDb}, {"ch_left", m_nChLeft}, {"ch_right", m_nChRight}};
            }

            /** mix into interleaved 64ch frames (capture drain thread) **/
            void Mix(float* pFrames, size_t nFrames)
            {
                std::lock_guard<std::mutex> lg(m_mutex);
                if(!m_bPlaying || !m_pSamples) { return; }
                const auto& vSamples = *m_pSamples;
                size_t nTotal = vSamples.size()/2;
                auto fGain = static_cast<float>(std::pow(10.0, m_dGainDb/20.0));
                bool bMono = (m_nChLeft == m_nChRight);
                for(size_t i = 0; i < nFrames; i++)
                {
                    if(m_nPos >= nTotal)
                    {
                        if(!m_bLoop) { m_bPlaying = false; m_nPos = 0; return; }
                        m_nPos = 0;
                    }
                    float fLeft = vSamples[m_nPos*2] * fGain;
                    float fRight = vSamples[m_nPos*2+1] * fGain;
                    if(bMono)
                    {
                        pFrames[i*kChannels + m_nChLeft] += 0.5f*(fLeft + fRight);
                    }
                    else
                    {
                        pFrames[i*kChannels + m_nChLeft] += fLeft;
                        pFrames[i*kChannels + m_nChRight] += fRight;
                    }
                    m_nPos++;
                }
            }

        private:
            mutable std::mutex m_mutex;
            std::shared_ptr<const std::vector<float>> m_pSamples;
            std::string m_sFile;
            size_t m_nPos = 0;
            bool m_bPlaying = false;
            bool m_bLoop = false;
            bool m_bAutostart = false;
            double m_dGainDb = 0.0;
            int m_nChLeft = 0;
            int m_nChRight = 1;
    };
}
