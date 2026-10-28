#include "log.h"
#include <chrono>
#include <cstdio>
#include <mutex>

using namespace pan;

namespace
{
    std::mutex g_mutex;
    const char* LEVELS[] = {"DBG", "INF", "WRN", "ERR"};
}

void log::Write(Level level, const std::string& sModule, const std::string& sMessage)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    struct tm tmLocal;
    localtime_r(&t, &tmLocal);

    std::lock_guard<std::mutex> lg(g_mutex);
    fprintf(stdout, "%02d:%02d:%02d.%03d %s [%s] %s\n",
            tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, static_cast<int>(ms),
            LEVELS[static_cast<int>(level)], sModule.c_str(), sMessage.c_str());
    fflush(stdout);
}
