#pragma once

#include <cstdint>
#include <vector>

// [Switch] SwitchNativeDecompress. The game's XMemDecompress (the Xbox 360 XDK's LZX decoder, statically
// linked, sub_831CE0D0) as native code. Only the case the game uses for its archives: a context without the
// streaming flag, which the guest decoder resets on every call, so the output depends on the input alone.
// The framing is the guest's (sub_831D6DF0): a 5-byte trailer after the last frame; frames of 32 KB behind a
// 2-byte big-endian compressed size, or behind 0xFF, a 2-byte uncompressed and a 2-byte compressed size (such
// a frame is the last); the bit buffer starts over at every frame; no pad byte after an odd-sized
// uncompressed block. Nothing depends on the platform, so it can be tested on the build machine.
namespace os::switch_lzx
{
    // Decodes `source` into `destination` as the guest would, where the guest writes (the sum of the frames'
    // sizes, from the frame headers, which `decodedSize` receives). Returns false for anything the guest could
    // decode differently (errors, which make the guest drop a frame or read stale window data, E8 translation,
    // sizes it does not handle), possibly after writing part of the output: the caller then runs the guest's
    // decoder, which starts over from the same input and writes the destination again.
    bool Decompress(const uint8_t* source, uint32_t sourceSize, uint32_t windowSize, uint8_t* destination, uint32_t& decodedSize);
}
