#include <os/switch_lzx.h>

#include <algorithm>
#include <cstring>

// See os/switch_lzx.h. A port of the guest's LZX decoder as it behaves on valid input (the same algorithm as
// the LZX of CAB files), checked against libmspack on the game's shader archive. Wherever the guest's result
// would depend on details this port does not reproduce (a frame it rejects, data read from before the start
// of the stream or past the source, E8 translation), Decompress gives up instead.
namespace
{
    // Per position slot: extra bits, and the base of the formatted offset minus 2 (the real offset).
    struct PositionSlots
    {
        uint8_t extraBits[52];
        uint32_t offsetBase[52];

        constexpr PositionSlots() : extraBits(), offsetBase()
        {
            for (uint32_t i = 0; i < 52; i++)
                extraBits[i] = uint8_t(i < 4 ? 0 : i < 36 ? i / 2 - 1 : 17);

            uint32_t base = 0;
            for (uint32_t i = 0; i < 52; i++)
            {
                offsetBase[i] = base - 2;
                base += 1u << extraBits[i];
            }
        }
    };

    constexpr PositionSlots SLOTS;

    constexpr uint32_t MAIN_MAX_SYMBOLS = 256 + 52 * 8;
    constexpr uint32_t LENGTH_SYMBOLS = 249;
    constexpr uint32_t PRETREE_SYMBOLS = 20;
    constexpr uint32_t ALIGNED_SYMBOLS = 8;

    struct DecodeError
    {
    };

    // Canonical Huffman codes of up to 16 bits, assigned by length then symbol. Codes of up to TABLE_BITS
    // bits are looked up directly; longer ones by their length's range of codes.
    template<uint32_t TABLE_BITS, uint32_t MAX_SYMBOLS>
    struct Huffman
    {
        // (symbol << 8) | length; length 0: no code starts this way; 0xFF: a longer code.
        uint32_t table[1u << TABLE_BITS];
        uint32_t firstCode[17];
        uint32_t count[17];
        uint32_t offset[17];
        uint16_t sorted[MAX_SYMBOLS];
        uint32_t maxLength;

        // Too many codes (a set of lengths no prefix code has) is an error; too few is not, as long as the
        // missing codes are never read.
        void Build(const uint8_t* lengths, uint32_t symbols)
        {
            memset(table, 0, sizeof(table));
            memset(count, 0, sizeof(count));
            maxLength = 0;
            for (uint32_t i = 0; i < symbols; i++)
            {
                if (lengths[i] > 16)
                    throw DecodeError();
                count[lengths[i]]++;
                maxLength = std::max<uint32_t>(maxLength, lengths[i]);
            }

            uint32_t code = 0;
            uint32_t sortedCount = 0;
            for (uint32_t length = 1; length <= 16; length++)
            {
                firstCode[length] = code;
                offset[length] = sortedCount;
                for (uint32_t symbol = 0; symbol < symbols; symbol++)
                {
                    if (lengths[symbol] != length)
                        continue;

                    if (code >= (1u << length))
                        throw DecodeError();

                    sorted[sortedCount++] = uint16_t(symbol);
                    if (length <= TABLE_BITS)
                    {
                        const uint32_t start = code << (TABLE_BITS - length);
                        const uint32_t end = (code + 1) << (TABLE_BITS - length);
                        for (uint32_t k = start; k < end; k++)
                            table[k] = (symbol << 8) | length;
                    }
                    else
                    {
                        table[code >> (length - TABLE_BITS)] = 0xFF;
                    }

                    code++;
                }
                code <<= 1;
            }
        }
    };

    class Decoder
    {
    public:
        Decoder(const uint8_t* source, size_t sourceSize, uint32_t windowSize, uint8_t* output, size_t outputSize)
            : source(source), sourceSize(sourceSize), windowSize(windowSize), output(output), outputSize(outputSize)
        {
            slots = 4;
            uint32_t reach = 4;
            while (reach < windowSize)
            {
                reach += 1u << SLOTS.extraBits[slots];
                slots++;
            }

            memset(mainLengths, 0, sizeof(mainLengths));
            memset(lengthLengths, 0, sizeof(lengthLengths));
        }

        // One frame of `todo` bytes, its compressed data at `frameStart`, `compressedSize` bytes long.
        void DecodeFrame(size_t frameStart, uint32_t compressedSize, uint32_t todo)
        {
            cur = frameStart;
            end = frameStart + compressedSize + 4;
            InitBitBuffer();
            DecodeData(todo);
        }

        size_t Position() const
        {
            return position;
        }

    private:
        const uint8_t* source;
        size_t sourceSize;
        uint32_t windowSize;
        uint8_t* output;
        size_t outputSize;
        size_t position = 0; // bytes decoded so far (the guest's window position, never wrapped here)

        uint32_t slots;
        uint8_t mainLengths[MAIN_MAX_SYMBOLS];
        uint8_t lengthLengths[LENGTH_SYMBOLS];
        Huffman<12, MAIN_MAX_SYMBOLS> mainTree;
        Huffman<12, LENGTH_SYMBOLS> lengthTree;
        Huffman<7, ALIGNED_SYMBOLS> alignedTree;
        Huffman<8, PRETREE_SYMBOLS> preTree;

        uint32_t repeated[3] = { 1, 1, 1 };
        bool newBlock = true;
        bool firstBlock = true;
        uint32_t blockType = 0;
        uint32_t blockRemaining = 0;

        // The bit buffer: its top 16 bits are always the next 16 bits of the stream, `bitCount` more follow.
        size_t cur = 0;
        size_t end = 0;
        uint32_t bitBuffer = 0;
        int32_t bitCount = 0;

        uint8_t Byte(size_t offset) const
        {
            if (offset >= sourceSize)
                throw DecodeError(); // The guest would read whatever follows the source.
            return source[offset];
        }

        void InitBitBuffer()
        {
            if (blockType == 3 || cur + 4 > end)
                return;

            bitBuffer = (uint32_t((Byte(cur + 1) << 8) | Byte(cur)) << 16) | uint32_t((Byte(cur + 3) << 8) | Byte(cur + 2));
            bitCount = 16;
            cur += 4;
        }

        void Fill(uint32_t bits)
        {
            bitBuffer <<= bits;
            bitCount -= int32_t(bits);
            for (int i = 0; i < 2; i++)
            {
                if (bitCount > 0)
                    return;
                if (cur >= end)
                    return;

                const uint32_t word = uint32_t(Byte(cur)) | (uint32_t(Byte(cur + 1)) << 8);
                cur += 2;
                // Only after the input ran out (at the end of a frame, the buffer may stop being refilled) can
                // the word land below the 32 bits.
                if (bitCount > -32)
                    bitBuffer |= word << uint32_t(-bitCount);
                bitCount += 16;
            }
        }

        uint32_t GetBits(uint32_t bits)
        {
            const uint32_t value = bitBuffer >> (32 - bits);
            Fill(bits);
            return value;
        }

        template<uint32_t TABLE_BITS, uint32_t MAX_SYMBOLS>
        uint32_t Decode(const Huffman<TABLE_BITS, MAX_SYMBOLS>& tree)
        {
            const uint32_t entry = tree.table[bitBuffer >> (32 - TABLE_BITS)];
            const uint32_t length = entry & 0xFF;
            if (length != 0 && length != 0xFF)
            {
                Fill(length);
                return entry >> 8;
            }

            if (length == 0)
                throw DecodeError();

            const uint32_t peek = bitBuffer >> 16;
            for (uint32_t codeLength = TABLE_BITS + 1; codeLength <= tree.maxLength; codeLength++)
            {
                const uint32_t code = peek >> (16 - codeLength);
                if (code - tree.firstCode[codeLength] < tree.count[codeLength])
                {
                    Fill(codeLength);
                    return tree.sorted[tree.offset[codeLength] + code - tree.firstCode[codeLength]];
                }
            }

            throw DecodeError();
        }

        void ReadLengths(uint8_t* lengths, uint32_t first, uint32_t last)
        {
            uint8_t preLengths[PRETREE_SYMBOLS];
            for (auto& length : preLengths)
                length = uint8_t(GetBits(4));
            preTree.Build(preLengths, PRETREE_SYMBOLS);

            for (uint32_t i = first; i < last;)
            {
                const uint32_t symbol = Decode(preTree);
                if (symbol == 17 || symbol == 18)
                {
                    uint32_t run = symbol == 17 ? 4 + GetBits(4) : 20 + GetBits(5);
                    for (; run != 0 && i < last; run--)
                        lengths[i++] = 0;
                }
                else if (symbol == 19)
                {
                    uint32_t run = 4 + GetBits(1);
                    const uint32_t delta = Decode(preTree);
                    if (delta > 16)
                        throw DecodeError();
                    const uint8_t value = uint8_t((lengths[i] + 17 - delta) % 17);
                    for (; run != 0 && i < last; run--)
                        lengths[i++] = value;
                }
                else
                {
                    lengths[i] = uint8_t((lengths[i] + 17 - symbol) % 17);
                    i++;
                }
            }
        }

        void DecodeCompressed(uint32_t amount, bool aligned)
        {
            size_t pos = position;
            const size_t stop = pos + amount;
            while (pos < stop)
            {
                if (cur >= end)
                    throw DecodeError();

                uint32_t symbol = Decode(mainTree);
                if (symbol < 256)
                {
                    output[pos++] = uint8_t(symbol);
                    continue;
                }

                symbol -= 256;
                uint32_t length = symbol & 7;
                if (length == 7)
                    length += Decode(lengthTree);
                length += 2;

                const uint32_t slot = symbol >> 3;
                uint32_t matchOffset;
                if (slot > 2)
                {
                    const uint32_t extra = SLOTS.extraBits[slot];
                    if (aligned && extra >= 3)
                    {
                        uint32_t value = extra > 3 ? GetBits(extra - 3) << 3 : 0;
                        value += Decode(alignedTree);
                        matchOffset = SLOTS.offsetBase[slot] + value;
                    }
                    else if (extra != 0)
                    {
                        matchOffset = SLOTS.offsetBase[slot] + GetBits(extra);
                    }
                    else
                    {
                        matchOffset = SLOTS.offsetBase[slot];
                    }

                    repeated[2] = repeated[1];
                    repeated[1] = repeated[0];
                    repeated[0] = matchOffset;
                }
                else
                {
                    matchOffset = repeated[slot];
                    repeated[slot] = repeated[0];
                    repeated[0] = matchOffset;
                }

                // Before the start of the stream (the guest reads its window's stale bytes), further back than
                // the window (it wraps), or past the frame (the guest fails the frame).
                if (matchOffset == 0 || matchOffset > pos || matchOffset >= windowSize || pos + length > stop)
                    throw DecodeError();

                const uint8_t* from = output + pos - matchOffset;
                uint8_t* to = output + pos;
                if (matchOffset >= length)
                {
                    memcpy(to, from, length);
                }
                else
                {
                    for (uint32_t k = 0; k < length; k++)
                        to[k] = from[k];
                }
                pos += length;
            }

            position = pos;
        }

        void DecodeUncompressed(uint32_t amount)
        {
            if (cur + amount > sourceSize)
                throw DecodeError();

            memcpy(output + position, source + cur, amount);
            cur += amount;
            position += amount;
        }

        void DecodeData(uint32_t todo)
        {
            if (position + todo > outputSize)
                throw DecodeError();

            while (todo > 0)
            {
                if (newBlock)
                {
                    if (firstBlock)
                    {
                        firstBlock = false;
                        // E8 translation: not reproduced.
                        if (GetBits(1) != 0)
                            throw DecodeError();
                    }

                    if (blockType == 3)
                    {
                        blockType = 0;
                        InitBitBuffer();
                    }

                    blockType = GetBits(3);
                    const uint32_t high = GetBits(8);
                    const uint32_t middle = GetBits(8);
                    const uint32_t low = GetBits(8);
                    blockRemaining = (high << 16) | (middle << 8) | low;

                    if (blockType == 2)
                    {
                        uint8_t alignedLengths[ALIGNED_SYMBOLS];
                        for (auto& length : alignedLengths)
                            length = uint8_t(GetBits(3));
                        alignedTree.Build(alignedLengths, ALIGNED_SYMBOLS);
                    }

                    if (blockType == 1 || blockType == 2)
                    {
                        ReadLengths(mainLengths, 0, 256);
                        ReadLengths(mainLengths, 256, 256 + slots * 8);
                        mainTree.Build(mainLengths, 256 + slots * 8);
                        ReadLengths(lengthLengths, 0, LENGTH_SYMBOLS);
                        lengthTree.Build(lengthLengths, LENGTH_SYMBOLS);
                    }
                    else if (blockType == 3)
                    {
                        // The guest backs up 2 bytes and reads R0-R2 as little-endian words; no alignment pad.
                        cur -= 2;
                        if (cur + 4 >= end)
                            throw DecodeError();

                        for (auto& offsetValue : repeated)
                        {
                            offsetValue = uint32_t(Byte(cur)) | (uint32_t(Byte(cur + 1)) << 8) |
                                (uint32_t(Byte(cur + 2)) << 16) | (uint32_t(Byte(cur + 3)) << 24);
                            cur += 4;
                        }
                    }
                    else
                    {
                        throw DecodeError();
                    }

                    newBlock = false;
                }

                while (blockRemaining > 0 && todo > 0)
                {
                    const uint32_t amount = std::min(blockRemaining, todo);
                    if (blockType == 3)
                        DecodeUncompressed(amount);
                    else
                        DecodeCompressed(amount, blockType == 2);

                    blockRemaining -= amount;
                    todo -= amount;
                }

                if (blockRemaining == 0)
                    newBlock = true;

                if (todo == 0)
                    InitBitBuffer();
            }
        }
    };

    uint32_t ReadBigEndian16(const uint8_t* data)
    {
        return (uint32_t(data[0]) << 8) | data[1];
    }
}

bool os::switch_lzx::Decompress(const uint8_t* source, uint32_t sourceSize, uint32_t windowSize, uint8_t* destination,
    uint32_t& decodedSize)
{
    if (sourceSize <= 5 || windowSize < (1u << 15) || windowSize > (1u << 21) || (windowSize & (windowSize - 1)) != 0)
        return false;

    // The frames, as the guest walks them (sub_831D6DF0).
    struct Frame
    {
        size_t start;
        uint32_t compressedSize;
        uint32_t size;
    };

    std::vector<Frame> frames;
    size_t total = 0;
    int64_t remaining = int64_t(sourceSize) - 5;
    size_t p = 0;
    while (remaining != 0)
    {
        if (p + 5 > sourceSize)
            return false;

        Frame frame;
        if (source[p] == 0xFF)
        {
            remaining -= 5;
            frame.size = ReadBigEndian16(source + p + 1);
            frame.compressedSize = ReadBigEndian16(source + p + 3);
            p += 5;
            if (int64_t(frame.compressedSize) < remaining)
                remaining = frame.compressedSize;
        }
        else
        {
            frame.compressedSize = ReadBigEndian16(source + p);
            frame.size = 32768;
            p += 2;
            remaining -= 2;
        }

        frame.start = p;
        frames.push_back(frame);
        total += frame.size;

        remaining -= frame.compressedSize;
        p += frame.compressedSize;
        if (remaining < 0 || p > sourceSize)
            return false;
    }

    if (total > UINT32_MAX)
        return false;

    // Matches are copied a byte at a time where they overlap, never past the frame.
    try
    {
        Decoder decoder(source, sourceSize, windowSize, destination, total);
        for (const Frame& frame : frames)
            decoder.DecodeFrame(frame.start, frame.compressedSize, frame.size);

        if (decoder.Position() != total)
            return false;
    }
    catch (const DecodeError&)
    {
        return false;
    }

    decodedSize = uint32_t(total);
    return true;
}
