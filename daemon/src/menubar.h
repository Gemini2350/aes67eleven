#pragma once
#include <functional>
#include <string>

namespace aes67vsc
{
    /** Runs the macOS menu bar presence (NSStatusItem) on the calling thread -
    *   must be the main thread. Returns when StopMenuBar() is called or the
    *   user picks quit (which first invokes onQuit so the engine can wind
    *   down). statusFn is polled every 2 s for the menu's status line. **/
    void RunMenuBar(std::function<void()> onQuit, std::function<std::string()> statusFn);

    /** thread-safe: ends RunMenuBar's loop **/
    void StopMenuBar();

    /** triggers the macos microphone permission prompt (required for reading
    *   audio input, even from our own virtual device) **/
    void RequestMicAccess(std::function<void(bool)> onDone);
}
