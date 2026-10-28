#pragma once
#include <functional>
#include <string>

namespace aes67vsc
{
    /** gateway build: no menu bar - RunMenuBar just parks the main thread
    *   until StopMenuBar() (from the housekeeping thread after a signal). **/
    void RunMenuBar(std::function<void()> onQuit, std::function<std::string()> statusFn);
    void StopMenuBar();
    void RequestMicAccess(std::function<void(bool)> onDone);
}
