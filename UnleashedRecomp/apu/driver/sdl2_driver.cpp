#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <os/logger.h>
#include <user/config.h>

#include <atomic>
#include <memory>

#if defined(__SWITCH__)
#include <pthread.h>
#include <switch.h>
#include <os/switch_cpu_profiler.h>
#endif

static PPCFunc* g_clientCallback{};
static uint32_t g_clientCallbackParam{}; // pointer in guest memory
static SDL_AudioDeviceID g_audioDevice{};
static bool g_audioDeviceReady{};
static bool g_downMixToStereo;

#if defined(__SWITCH__)
// [Switch] The game's audio goes to the console through audout, fed straight from the pump thread
// (priority 0x20), instead of through SDL.
//
// SDL's Switch backend plays from its own thread, and SDL starts that thread with
// SDL_THREAD_PRIORITY_TIME_CRITICAL, which devkitPro's SDL maps to Horizon priority 0x3B: the lowest
// normal priority, below every game thread and the render thread. Whenever the CPU was busy it got no
// time. Worse, after queuing a buffer its play loop waits until that buffer is reported *playing*; a
// thread that was starved for longer than a buffer (21 ms) finds it already *done*, and then waits
// forever. From then on nothing was played, the pump saw the queue full and stopped calling the game's
// mixer: silence until the game was restarted, more likely the more CPU-bound the scene.
//
// audout keeps its own queue in the audio service. An underrun is a short gap, and playback resumes
// with the next buffer. The samples are converted exactly as SDL converted them (F32 to S16).
static constexpr uint32_t AUDOUT_BUFFER_COUNT = 16;
static constexpr size_t AUDOUT_BLOCK_BYTES = XAUDIO_NUM_SAMPLES * 2 * sizeof(int16_t);
static constexpr size_t AUDOUT_BUFFER_BYTES = 0x1000; // audout wants 0x1000-aligned buffers and sizes
static_assert(AUDOUT_BLOCK_BYTES <= AUDOUT_BUFFER_BYTES);

static AudioOutBuffer g_audoutBuffers[AUDOUT_BUFFER_COUNT];
static bool g_audoutQueuedFlags[AUDOUT_BUFFER_COUNT];
static uint8_t* g_audoutMemory = nullptr;
static uint32_t g_audoutQueued = 0;
static uint64_t g_audoutAppended = 0;
static bool g_audoutReady = false;
static bool g_audoutStarted = false;

// Times the service ran out of blocks to play (a short gap each). Read by the GPU pass profiler report.
std::atomic<uint32_t> g_switchAudioUnderruns{ 0 };

static void CreateAudoutDevice()
{
    Result rc = audoutInitialize();
    if (R_FAILED(rc))
    {
        LOGFN_ERROR("audoutInitialize failed: 0x{:X}", rc);
        return;
    }

    rc = audoutStartAudioOut();
    if (R_FAILED(rc))
    {
        LOGFN_ERROR("audoutStartAudioOut failed: 0x{:X}", rc);
        audoutExit();
        return;
    }

    g_audoutMemory = static_cast<uint8_t*>(aligned_alloc(AUDOUT_BUFFER_BYTES, AUDOUT_BUFFER_COUNT * AUDOUT_BUFFER_BYTES));
    if (g_audoutMemory == nullptr)
    {
        LOGN_ERROR("Could not allocate the audout buffers.");
        audoutStopAudioOut();
        audoutExit();
        return;
    }

    memset(g_audoutMemory, 0, AUDOUT_BUFFER_COUNT * AUDOUT_BUFFER_BYTES);
    for (uint32_t i = 0; i < AUDOUT_BUFFER_COUNT; i++)
    {
        g_audoutBuffers[i].next = nullptr;
        g_audoutBuffers[i].buffer = g_audoutMemory + i * AUDOUT_BUFFER_BYTES;
        g_audoutBuffers[i].buffer_size = AUDOUT_BUFFER_BYTES;
        g_audoutBuffers[i].data_size = AUDOUT_BLOCK_BYTES;
        g_audoutBuffers[i].data_offset = 0;
        g_audoutQueuedFlags[i] = false;
    }

    // audout is stereo, 48 kHz, 16-bit.
    g_downMixToStereo = true;
    g_audoutReady = true;

    fprintf(stderr, "Switch audio: audout %u Hz, %u channels, %u-sample blocks fed by the pump thread\n",
        audoutGetSampleRate(), audoutGetChannelCount(), uint32_t(XAUDIO_NUM_SAMPLES));
}

// Takes back every block the service has played. Pump thread only.
static void ReclaimAudoutBuffers()
{
    while (g_audoutQueued != 0)
    {
        AudioOutBuffer* released = nullptr;
        uint32_t releasedCount = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &releasedCount)) || releasedCount == 0 || released == nullptr)
            break;

        const size_t index = size_t(released - g_audoutBuffers);
        if (index < AUDOUT_BUFFER_COUNT && g_audoutQueuedFlags[index])
        {
            g_audoutQueuedFlags[index] = false;
            g_audoutQueued--;
        }
    }
}

// SDL_Convert_F32_to_S16, which is what the SDL path applied to these samples.
static inline int16_t ConvertSampleToS16(float sample)
{
    if (sample >= 1.0f)
        return 32767;
    if (sample <= -1.0f)
        return -32768;
    return int16_t(sample * 32767.0f);
}
#endif

static void CreateAudioDevice()
{
    if (g_audioDevice != NULL)
    {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = 0;
        g_audioDeviceReady = false;
    }

    bool surround = Config::ChannelConfiguration == EChannelConfiguration::Surround;
    int allowedChanges = surround ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0;

    SDL_AudioSpec desired{}, obtained{};
    desired.freq = XAUDIO_SAMPLES_HZ;
    desired.format = AUDIO_F32SYS;
    desired.channels = surround ? XAUDIO_NUM_CHANNELS : 2;
#if defined(__SWITCH__)
    // Give audren device-side headroom; a single 256-sample period is too tight
    // for the shared cores.
    desired.samples = XAUDIO_NUM_SAMPLES * 4;
#else
    desired.samples = XAUDIO_NUM_SAMPLES;
#endif
    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, allowedChanges);

    if (g_audioDevice && obtained.channels != 2 && obtained.channels != XAUDIO_NUM_CHANNELS) // This check may fail only when surround sound is enabled.
    {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = 0;
        obtained = {};
        g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    }

    if (!g_audioDevice)
    {
        LOGFN_ERROR("Failed to open audio device: {}", SDL_GetError());
        g_downMixToStereo = true;
        g_audioDeviceReady = false;
        return;
    }

    g_audioDeviceReady = true;
    g_downMixToStereo = (obtained.channels == 2);
}

void XAudioInitializeSystem()
{
#if defined(__SWITCH__)
    // [Switch] SwitchAudioOut = false (or audout failing to start) keeps the SDL path.
    if (Config::SwitchAudioOut)
    {
        CreateAudoutDevice();
        if (g_audoutReady)
            return;
    }
#endif

#ifdef _WIN32
    // Force wasapi on Windows.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Unleashed Recompiled");

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        LOGFN_ERROR("Failed to init audio subsystem: {}", SDL_GetError());
        return;
    }

    CreateAudioDevice();
}

#if defined(__SWITCH__)
static pthread_t g_audioThread{};
static bool g_audioThreadCreated{};
#else
static std::unique_ptr<std::thread> g_audioThread;
#endif
static std::atomic<bool> g_audioThreadShouldExit;

#if defined(__SWITCH__)
static void* AudioThread(void*)
#else
static void AudioThread()
#endif
{
    using namespace std::chrono_literals;

    std::unique_ptr<GuestThreadContext> ctx;

    size_t channels = g_downMixToStereo ? 2 : XAUDIO_NUM_CHANNELS;

#if defined(__SWITCH__)
    // Keep the pump above the game's bulk worker threads so it holds its cadence.
    svcSetThreadPriority(threadGetCurHandle(), 0x20);
    os::switch_cpu_profiler::RegisterCurrentThread("audio pump");

    // audout needs a few blocks queued to ride out a late tick; the queue is topped up to this many
    // (16 ms) whenever it runs low, and never grows past MAX_LATENCY blocks.
    constexpr uint32_t TARGET_QUEUED_BLOCKS = 3;

    // Absolute deadline advanced by exactly one interval per tick: the reference
    // pump's fixed-rate 187.5Hz schedule (0x137fa0). Driving the guest mixer at a
    // steady one-block-per-tick is what keeps its voice buffers full.
    constexpr auto PUMP_INTERVAL = std::chrono::nanoseconds(1000000000ll * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ);
    auto pumpDeadline = std::chrono::steady_clock::now();
#endif

    while (!g_audioThreadShouldExit.load(std::memory_order_acquire))
    {
        constexpr size_t MAX_LATENCY = 10;
#if defined(__SWITCH__)
        if (g_audoutReady)
        {
            ReclaimAudoutBuffers();

            if (g_audoutStarted && g_audoutQueued == 0)
                g_switchAudioUnderruns.fetch_add(1, std::memory_order_relaxed);

            // One block per tick, as before; more only to rebuild the cushion after a late tick or a gap.
            const uint32_t blocksToRender = g_audoutQueued >= TARGET_QUEUED_BLOCKS ? 1 : TARGET_QUEUED_BLOCKS - g_audoutQueued;

            for (uint32_t block = 0; block < blocksToRender; block++)
            {
                if (g_audoutQueued > MAX_LATENCY)
                    break;

                if (g_clientCallback == nullptr)
                    break;

                if (ctx == nullptr)
                    ctx = std::make_unique<GuestThreadContext>(0);

                const uint64_t appendedBefore = g_audoutAppended;
                ctx->ppcContext.r3.u32 = g_clientCallbackParam;
                g_clientCallback(ctx->ppcContext, g_memory.base);

                // A mixer call that submitted nothing: no catch-up this tick.
                if (g_audoutAppended == appendedBefore)
                    break;
            }
        }
        else
#endif
        {
            uint32_t queuedAudioSize = g_audioDevice ? SDL_GetQueuedAudioSize(g_audioDevice) : 0;
            const size_t callbackAudioSize = channels * XAUDIO_NUM_SAMPLES * sizeof(float);

            if ((queuedAudioSize / callbackAudioSize) <= MAX_LATENCY && g_clientCallback != nullptr)
            {
                if (ctx == nullptr)
                    ctx = std::make_unique<GuestThreadContext>(0);

                ctx->ppcContext.r3.u32 = g_clientCallbackParam;
                g_clientCallback(ctx->ppcContext, g_memory.base);
            }
        }

#if defined(__SWITCH__)
        pumpDeadline += PUMP_INTERVAL;
        auto now = std::chrono::steady_clock::now();
        if (now >= pumpDeadline)
            pumpDeadline = now + PUMP_INTERVAL;
        else
            std::this_thread::sleep_until(pumpDeadline);
#else
        auto now = std::chrono::steady_clock::now();
        constexpr auto INTERVAL = 1000000000ns * XAUDIO_NUM_SAMPLES / XAUDIO_SAMPLES_HZ;
        auto next = now + (INTERVAL - now.time_since_epoch() % INTERVAL);

        std::this_thread::sleep_for(std::chrono::floor<std::chrono::milliseconds>(next - now));

        while (std::chrono::steady_clock::now() < next)
            std::this_thread::yield();
#endif
    }
}

static void CreateAudioThread()
{
#if defined(__SWITCH__)
    if (!g_audoutReady)
#endif
    SDL_PauseAudioDevice(g_audioDevice, 0);
    g_audioThreadShouldExit = false;
#if defined(__SWITCH__)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    constexpr auto AUDIO_THREAD_STACK_SIZE = 2 * 1024 * 1024;
    const auto stackResult = pthread_attr_setstacksize(&attr, AUDIO_THREAD_STACK_SIZE);
    if (stackResult != 0)
        LOGFN_ERROR("Switch XAudio pthread_attr_setstacksize failed: 0x{:X}", stackResult);

    const auto createResult = pthread_create(&g_audioThread, &attr, AudioThread, nullptr);
    pthread_attr_destroy(&attr);
    if (createResult != 0)
    {
        LOGFN_ERROR("Switch XAudio pthread_create failed: 0x{:X}", createResult);
        return;
    }

    g_audioThreadCreated = true;
#else
    g_audioThread = std::make_unique<std::thread>(AudioThread);
#endif
}

void XAudioRegisterClient(PPCFunc* callback, uint32_t param)
{
    auto* pClientParam = static_cast<uint32_t*>(g_userHeap.Alloc(sizeof(param)));
    ByteSwapInplace(param);
    *pClientParam = param;
    g_clientCallbackParam = g_memory.MapVirtual(pClientParam);
    g_clientCallback = callback;

    CreateAudioThread();
}

void XAudioSubmitFrame(void* samples)
{
    auto floatSamples = reinterpret_cast<be<float>*>(samples);

#if defined(__SWITCH__)
    // Called by the game's mixer, on the pump thread.
    if (g_audoutReady)
    {
        uint32_t index = 0;
        while (index < AUDOUT_BUFFER_COUNT && g_audoutQueuedFlags[index])
            index++;

        if (index == AUDOUT_BUFFER_COUNT)
            return; // Every block is queued (the pump never lets that happen).

        auto* out = static_cast<int16_t*>(g_audoutBuffers[index].buffer);
        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            // The same stereo downmix as below.
            float ch0 = floatSamples[0 * XAUDIO_NUM_SAMPLES + i];
            float ch1 = floatSamples[1 * XAUDIO_NUM_SAMPLES + i];
            float ch2 = floatSamples[2 * XAUDIO_NUM_SAMPLES + i];
            float ch4 = floatSamples[4 * XAUDIO_NUM_SAMPLES + i];
            float ch5 = floatSamples[5 * XAUDIO_NUM_SAMPLES + i];

            out[i * 2 + 0] = ConvertSampleToS16((ch0 + ch2 * 0.75f + ch4) * Config::MasterVolume);
            out[i * 2 + 1] = ConvertSampleToS16((ch1 + ch2 * 0.75f + ch5) * Config::MasterVolume);
        }

        g_audoutBuffers[index].data_size = AUDOUT_BLOCK_BYTES;
        g_audoutBuffers[index].data_offset = 0;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_audoutBuffers[index])))
        {
            g_audoutQueuedFlags[index] = true;
            g_audoutQueued++;
            g_audoutAppended++;
            g_audoutStarted = true;
        }
        return;
    }
#endif

    if (g_downMixToStereo)
    {
        // 0: left 1.0f, right 0.0f
        // 1: left 0.0f, right 1.0f
        // 2: left 0.75f, right 0.75f
        // 3: left 0.0f, right 0.0f
        // 4: left 1.0f, right 0.0f
        // 5: left 0.0f, right 1.0f

        std::array<float, 2 * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            float ch0 = floatSamples[0 * XAUDIO_NUM_SAMPLES + i];
            float ch1 = floatSamples[1 * XAUDIO_NUM_SAMPLES + i];
            float ch2 = floatSamples[2 * XAUDIO_NUM_SAMPLES + i];
            float ch3 = floatSamples[3 * XAUDIO_NUM_SAMPLES + i];
            float ch4 = floatSamples[4 * XAUDIO_NUM_SAMPLES + i];
            float ch5 = floatSamples[5 * XAUDIO_NUM_SAMPLES + i];

            audioFrames[i * 2 + 0] = (ch0 + ch2 * 0.75f + ch4) * Config::MasterVolume;
            audioFrames[i * 2 + 1] = (ch1 + ch2 * 0.75f + ch5) * Config::MasterVolume;
        }

        SDL_QueueAudio(g_audioDevice, &audioFrames, sizeof(audioFrames));
    }
    else
    {
        std::array<float, XAUDIO_NUM_CHANNELS * XAUDIO_NUM_SAMPLES> audioFrames;

        for (size_t i = 0; i < XAUDIO_NUM_SAMPLES; i++)
        {
            for (size_t j = 0; j < XAUDIO_NUM_CHANNELS; j++)
                audioFrames[i * XAUDIO_NUM_CHANNELS + j] = floatSamples[j * XAUDIO_NUM_SAMPLES + i] * Config::MasterVolume;
        }

        SDL_QueueAudio(g_audioDevice, &audioFrames, sizeof(audioFrames));
    }
}
