#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include "bmca.h"
#include "json.hpp"

namespace pan::ptp
{
    /** IEEE 1588-2008 ordinary clock, slave only, end-to-end delay mechanism.
    *   Full BMCA over every announce source seen on the domain; exposes the
    *   complete foreign master list plus the reason each master won or lost -
    *   that is the data behind the analyzer page.
    *
    *   Media time: PtpTimeNs() = TAI nanoseconds since the ptp epoch, derived
    *   from the monotonic host clock plus a smoothed offset. Good to well under
    *   a millisecond with software timestamps - plenty for AES67 media clocks.
    **/
    class PtpClient
    {
        public:
            PtpClient() = default;
            ~PtpClient();

            /** clock sync runs on the first interface; a second one is monitored
            *   for announces only, so amber and blue ptp can be compared **/
            bool Run(const std::vector<std::string>& vInterfaces, int nDomain);
            void Stop();
            void SetDomain(int nDomain);        //restarts the session state
            /** auto: hop to whichever domain actually carries announces when
            *   the configured one stays silent (manual value = fallback) **/
            void SetAutoDomain(bool bAuto) { m_bAutoDomain = bAuto; }
            int Domain() const { return m_nDomain.load(); }

            bool IsSynced() const { return m_bSynced.load(); }
            uint64_t PtpTimeNs() const;         //0 if never synced
            //ptp time -> host clock via the current mapping (for absolute deadline sleeps)
            uint64_t PtpToHostNs(uint64_t nPtpNs) const
            {
                return uint64_t(int64_t(nPtpNs) - m_nMappingNs.load());
            }
            std::string GrandmasterId() const;

            /** host->ptp mapping snapshot for disciplining the driver clock:
            *   ptp(t) = nAnchorPtpNs + (t - nAnchorHostNs) * dRate **/
            struct Mapping
            {
                uint64_t nAnchorHostNs = 0;
                uint64_t nAnchorPtpNs = 0;
                double dRate = 1.0;         //d(ptp)/d(host)
                bool bLocked = false;
            };
            Mapping GetMapping() const;

            nlohmann::json GetStatusJson() const;   //analyzer + status data for ui/nmos

        private:
            struct ForeignMaster
            {
                AnnounceDataset dataset;    //latest from the primary net (fallback: secondary)
                uint64_t nAnnounceCount = 0;                //primary net - bmca qualification
                std::array<uint64_t, 2> aAnnounceCount{};   //per interface
                std::array<AnnounceDataset, 2> aDataset{};  //per interface, for the meta compare
                std::chrono::steady_clock::time_point lastSeen;
                std::chrono::steady_clock::time_point firstSeen;
                std::string sAddress;
                std::string sBmcaInfo;      //"selected" or "loses to <id> on <field>"
                bool bSelected = false;
            };

            void RxLoop();
            void TimerLoop();
            void HandleMessage(const uint8_t* pData, size_t nSize, uint64_t nRxNs,
                               const std::string& sFrom, size_t nInterface);
            void HandleAnnounce(const uint8_t* pData, size_t nSize, const std::string& sFrom, size_t nInterface);
            void HandleSync(const uint8_t* pData, size_t nSize, uint64_t nRxNs);
            void HandleFollowUp(const uint8_t* pData, size_t nSize);
            void HandleDelayResp(const uint8_t* pData, size_t nSize);
            void RunBmca();
            void SendDelayReq();
            void CompleteMeasurement(uint64_t nT1, uint64_t nT2);
            void AddOffsetSample(double dOffsetNs);

            bool OpenSockets();
            void CloseSockets();

            std::vector<std::string> m_vInterfaces;
            std::atomic<int> m_nDomain{0};
            std::atomic<bool> m_bAutoDomain{true};
            //announce activity per domain (guarded by m_mutex) for auto detection
            std::map<int, std::pair<uint64_t, std::chrono::steady_clock::time_point>> m_mDomainSeen;
            std::array<int, 2> m_anEventSocket{-1, -1};     //319, per interface
            std::array<int, 2> m_anGeneralSocket{-1, -1};   //320, per interface
            std::array<int, 2> m_anIfIndex{0, 0};           //for IP_PKTINFO attribution
            ClockIdentity m_ownIdentity{};

            std::thread m_rxThread;
            std::thread m_timerThread;
            std::atomic<bool> m_bRun{false};

            mutable std::mutex m_mutex;
            std::map<PortIdentity, ForeignMaster> m_mForeign;
            std::optional<PortIdentity> m_selectedMaster;

            //sync measurement state (guarded by m_mutex)
            uint16_t m_nSyncSeq = 0;
            uint64_t m_nSyncRxNs = 0;           //t2 for the sync we await a follow-up for
            bool m_bAwaitFollowUp = false;
            uint16_t m_nDelaySeq = 0;
            uint64_t m_nDelayReqTxNs = 0;       //t3
            double m_dMeanPathDelayNs = 0.0;
            std::deque<double> m_qDelayWindow;  //rolling-min filter (one-sided noise)
            bool m_bHaveDelay = false;

            //servo / mapping (guarded by m_mutex)
            std::deque<double> m_qOffsetWindow;
            double m_dSmoothedOffsetNs = 0.0;   //ptp - host clock
            std::atomic<bool> m_bSynced{false};
            std::atomic<int64_t> m_nMappingNs{0};   //atomic copy of smoothed offset for PtpTimeNs
            uint64_t m_nSyncCount = 0;
            uint64_t m_nLastSampleHostNs = 0;
            int m_nConsecutiveRejects = 0;
            uint64_t m_nRejectedOutliers = 0;
            //rate of the ptp clock against the host crystal, from the drift of
            //the smoothed offset over a sliding window of (host ns, offset ns)
            std::deque<std::pair<uint64_t, double>> m_qRateWindow;
            double m_dRate = 1.0;
            std::deque<std::pair<uint64_t, double>> m_qOffsetHistory;   //(ptp ms, offset ns) for the ui chart
            std::chrono::steady_clock::time_point m_lastSyncSeen{};
    };
}
