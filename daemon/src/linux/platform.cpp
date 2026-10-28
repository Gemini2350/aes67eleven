#include "menubar.h"
#include <condition_variable>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    std::condition_variable g_cv;
    bool g_bQuit = false;
}

void aes67vsc::RunMenuBar(std::function<void()>, std::function<std::string()>)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return g_bQuit; });
}

void aes67vsc::StopMenuBar()
{
    {
        std::lock_guard<std::mutex> lg(g_mutex);
        g_bQuit = true;
    }
    g_cv.notify_all();
}

void aes67vsc::RequestMicAccess(std::function<void(bool)> onDone)
{
    if(onDone) { onDone(true); }
}
