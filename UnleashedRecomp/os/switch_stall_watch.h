#pragma once

#if defined(__SWITCH__)

#include <string>

// [Switch] Stall watchdog ([Switch] SwitchStallWatchSeconds). A thread of its own notices when no frame has
// been presented for that long and writes to stderr.log, as "[stall]" lines, what every registered thread
// (os::switch_cpu_profiler) is doing: whether it ran since the last look, where it is (program counter,
// return address, frame-pointer chain and the return addresses on its stack, as offsets into the executable
// that tools/switch-cpu-profile.py names), plus the renderer's queues. It looks again every few seconds while
// the stall lasts and writes its length once frames resume. Shorter stutters are counted: a "[hitch]" line
// sums up, at most once a second, the frames that took over 100 ms. Idle otherwise: one check every 250 ms.
namespace os::switch_stall_watch
{
    void Start(double seconds);

    // Called once per presented frame (the game's main thread).
    void OnFrame();

    // Frames presented so far (counted with the watchdog off too).
    uint64_t FrameCount();

    // Extra state printed with each dump. Called on the watchdog thread; must not block or allocate while
    // other threads could be paused (it is only called once they run again).
    void SetStateReporter(void (*reporter)(std::string& out));
}

#endif
