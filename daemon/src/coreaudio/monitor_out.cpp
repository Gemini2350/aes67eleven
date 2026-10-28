#include "monitor_out.h"
#include <algorithm>
#include <cmath>
#include "driver_iface.h"
#include "log.h"

using namespace aes67vsc;
using json = nlohmann::json;

bool MonitorOut::Start(const std::string& sDevice, double dGainDb)
{
    Stop();
    m_sDevice = sDevice;
    SetGainDb(dGainDb);

    //monitoring onto our own devices would howl
    if(sDevice == kDeviceUidMain || sDevice == kDeviceUidBridge ||
       sDevice == "AES67" || sDevice == "AES67eleven" || sDevice == "AES67 Bridge")
    {
        LOG_ERROR("monitor") << "refusing to monitor onto " << sDevice;
        return false;
    }

    if(!m_device.OpenOutput(sDevice, 256))
    {
        LOG_ERROR("monitor") << "output device '" << sDevice << "' not found";
        return false;
    }

    m_ring.Clear();
    m_bRun = true;
    bool bOk = m_device.Start([this](const AudioBufferList*, AudioBufferList* pOutput)
    {
        if(!pOutput || pOutput->mNumberBuffers == 0) { return; }
        auto& buffer = pOutput->mBuffers[0];
        auto* pOut = static_cast<float*>(buffer.mData);
        size_t nChannels = buffer.mNumberChannels > 0 ? buffer.mNumberChannels : 2;
        size_t nFrames = buffer.mDataByteSize / (nChannels * sizeof(float));

        //consumer-side trim keeps the monitor latency near one io buffer pair
        //even though the speaker crystal drifts against the ptp-paced producer
        if(m_ring.FillFrames() > 2048)
        {
            m_nTrimmed += m_ring.DropFrames(m_ring.FillFrames() - 1024);
        }

        float aFrame[2];
        for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
        {
            if(m_ring.PopFrames(aFrame, 1) < 1)
            {
                aFrame[0] = aFrame[1] = 0.f;
                //idle silence is not an underrun - only count while being fed
                if(nFrame == 0 &&
                   clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - m_nLastFeedNs.load(std::memory_order_relaxed)
                       < 1'000'000'000ULL)
                {
                    m_nUnderruns++;
                }
            }
            float* pFrame = pOut + nFrame*nChannels;
            pFrame[0] = aFrame[0];
            if(nChannels > 1) { pFrame[1] = aFrame[1]; }
            for(size_t nCh = 2; nCh < nChannels; nCh++) { pFrame[nCh] = 0.f; }
        }
    });
    if(!bOk)
    {
        m_bRun = false;
        m_device.Close();
        LOG_ERROR("monitor") << "could not start io on '" << sDevice << "'";
        return false;
    }
    LOG_INFO("monitor") << "monitoring on '" << sDevice << "' gain " << dGainDb << " dB";
    return true;
}

void MonitorOut::Stop()
{
    m_bRun = false;
    m_device.Close();
}

void MonitorOut::SetChannels(int nLeft, int nRight)
{
    m_nLeft = std::clamp(nLeft < 0 ? 0 : nLeft, 0, int(kChannels)-1);
    m_nRight = std::clamp(nRight < 0 ? 1 : nRight, 0, int(kChannels)-1);
}

void MonitorOut::SetGainDb(double dGainDb)
{
    m_fGain = static_cast<float>(std::pow(10.0, dGainDb/20.0));
}

void MonitorOut::Feed(const float* pInterleaved, size_t nChannels, size_t nFrames)
{
    if(!m_bRun.load(std::memory_order_relaxed) || nChannels == 0) { return; }
    m_nLastFeedNs.store(clock_gettime_nsec_np(CLOCK_UPTIME_RAW), std::memory_order_relaxed);
    float fGain = m_fGain.load(std::memory_order_relaxed);
    auto nLeft = std::min(static_cast<size_t>(m_nLeft.load()), nChannels-1);
    auto nRight = std::min(static_cast<size_t>(m_nRight.load()), nChannels-1);

    float aFrame[2];
    for(size_t nFrame = 0; nFrame < nFrames; nFrame++)
    {
        aFrame[0] = pInterleaved[nFrame*nChannels + nLeft] * fGain;
        aFrame[1] = pInterleaved[nFrame*nChannels + nRight] * fGain;
        //ring full: drop the incoming frame; the io proc's trim recovers latency
        if(m_ring.PushFrames(aFrame, 1) < 1) { return; }
    }
}

json MonitorOut::GetStatusJson() const
{
    return {
        {"running", m_bRun.load()},
        {"device", m_sDevice},
        {"fill_frames", m_ring.FillFrames()},
        {"underruns", m_nUnderruns.load()},
        {"trimmed", m_nTrimmed.load()},
        {"channels", {m_nLeft.load(), m_nRight.load()}}
    };
}
