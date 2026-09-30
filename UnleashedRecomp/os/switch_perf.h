#pragma once

#if defined(__SWITCH__)

namespace os::switch_perf
{
    // Asks Horizon for the stock 460.8 MHz handheld GPU profile (0x92220008: GPU 460.8 MHz,
    // memory 1331.2 MHz) on a background thread, verifies it, and reverts it if the memory
    // clock moved. Does nothing when the console starts docked. Not an overclock: it is the
    // same apm configuration commercial games request.
    void StartHandheldGpuBoost();
}

#endif
