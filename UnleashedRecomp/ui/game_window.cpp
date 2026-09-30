#include "game_window.h"
#include <gpu/video.h>
#include <os/logger.h>
#include <os/user.h>
#include <os/version.h>
#include <app.h>
#include <sdl_listener.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#if !defined(__SWITCH__)
#include <SDL_syswm.h>
#endif


#if _WIN32
#include <dwmapi.h>
#include <shellscalingapi.h>
#endif

#include <res/images/game_icon.bmp.h>
#include <res/images/game_icon_night.bmp.h>

bool m_isFullscreenKeyReleased = true;
bool m_isResizing = false;

#if defined(__SWITCH__)
namespace
{
    // [Switch] The window follows the console's output: 1920x1080 docked, 1280x720 in handheld mode. The default
    // display resolution tells which without an applet message loop (none runs here, so appletGetOperationMode()
    // would keep its first value); a dock set to 720p counts as handheld. Its change event says when to look again.
    // The Resolution Scale option is kept per mode ([Switch] SwitchDockedResolutionScale 0.8 = 864p,
    // SwitchHandheldResolutionScale 0.9 = 648p).
    constexpr int DOCKED_WIDTH = 1920;
    constexpr int DOCKED_HEIGHT = 1080;

    Event g_displayResolutionEvent{};
    bool g_displayResolutionEventValid = false;
    bool g_docked = false;

    bool QueryDocked()
    {
        s32 width = 0;
        s32 height = 0;
        if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&width, &height)) && height > 0)
            return height >= DOCKED_HEIGHT;

        return appletGetOperationMode() == AppletOperationMode_Console;
    }

    // The mode's scale becomes the option's value, and its default what the option's reset gives.
    void ApplyModeResolutionScale(bool docked)
    {
        auto& modeScale = docked ? Config::SwitchDockedResolutionScale : Config::SwitchHandheldResolutionScale;
        modeScale.Value = std::clamp(modeScale.Value, 0.25f, 2.0f);
        Config::ResolutionScale.DefaultValue = modeScale.DefaultValue;
        Config::ResolutionScale.Value = modeScale.Value;
    }
}

void GameWindow::SetSwitchModeResolutionScale(float scale)
{
    auto& modeScale = g_docked ? Config::SwitchDockedResolutionScale : Config::SwitchHandheldResolutionScale;
    modeScale.Value = std::clamp(scale, 0.25f, 2.0f);
}
#endif

int Window_OnSDLEvent(void*, SDL_Event* event)
{
    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
        ImGui_ImplSDL2_ProcessEvent(event);

    for (auto listener : GetEventListeners())
    {
        if (listener->OnSDLEvent(event))
        {
            return 0;
        }
    }

    switch (event->type)
    {
        case SDL_QUIT:
        {
            if (App::s_isSaving)
                break;

            App::Exit();

            break;
        }

        case SDL_KEYDOWN:
        {
            switch (event->key.keysym.sym)
            {
                // Toggle fullscreen on ALT+ENTER.
                case SDLK_RETURN:
                {
                    if (!(event->key.keysym.mod & KMOD_ALT) || !m_isFullscreenKeyReleased)
                        break;

                    Config::Fullscreen = GameWindow::SetFullscreen(!GameWindow::IsFullscreen());

                    if (Config::Fullscreen)
                    {
                        Config::Monitor = GameWindow::GetDisplay();
                    }
                    else
                    {
                        Config::WindowState = GameWindow::SetMaximised(Config::WindowState == EWindowState::Maximised);
                    }

                    // Block holding ALT+ENTER spamming window changes.
                    m_isFullscreenKeyReleased = false;

                    break;
                }

                // Restore original window dimensions on F2.
                case SDLK_F2:
                    Config::Fullscreen = GameWindow::SetFullscreen(false);
                    GameWindow::ResetDimensions();
                    break;

                // Recentre window on F3.
                case SDLK_F3:
                {
                    if (GameWindow::IsFullscreen())
                        break;

                    GameWindow::SetDimensions(GameWindow::s_width, GameWindow::s_height);

                    break;
                }
            }

            break;
        }

        case SDL_KEYUP:
        {
            switch (event->key.keysym.sym)
            {
                // Allow user to input ALT+ENTER again.
                case SDLK_RETURN:
                    m_isFullscreenKeyReleased = true;
                    break;
            }
        }

        case SDL_WINDOWEVENT:
        {
            switch (event->window.event)
            {
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    GameWindow::s_isFocused = false;
                    SDL_ShowCursor(SDL_ENABLE);
                    break;

                case SDL_WINDOWEVENT_FOCUS_GAINED:
                {
                    GameWindow::s_isFocused = true;

                    if (GameWindow::IsFullscreen())
                        SDL_ShowCursor(GameWindow::s_isFullscreenCursorVisible ? SDL_ENABLE : SDL_DISABLE);

                    break;
                }

                case SDL_WINDOWEVENT_RESTORED:
                    Config::WindowState = EWindowState::Normal;
                    break;

                case SDL_WINDOWEVENT_MAXIMIZED:
                    Config::WindowState = EWindowState::Maximised;
                    break;

                case SDL_WINDOWEVENT_RESIZED:
                    m_isResizing = true;
                    Config::WindowSize = -1;
                    GameWindow::s_width = event->window.data1;
                    GameWindow::s_height = event->window.data2;
                    GameWindow::SetTitle(fmt::format("{} - [{}x{}]", GameWindow::GetTitle(), GameWindow::s_width, GameWindow::s_height).c_str());
                    break;

                case SDL_WINDOWEVENT_MOVED:
                    GameWindow::s_x = event->window.data1;
                    GameWindow::s_y = event->window.data2;
                    break;
            }

            break;
        }

        case SDL_USER_EVILSONIC:
            GameWindow::s_isIconNight = event->user.code;
            GameWindow::SetIcon(GameWindow::s_isIconNight);
            break;
    }

    return 0;
}

void GameWindow::Init(const char* sdlVideoDriver)
{
#if defined(__SWITCH__)
    (void)sdlVideoDriver;

    g_docked = QueryDocked();

    s_x = 0;
    s_y = 0;
    s_width = g_docked ? DOCKED_WIDTH : DEFAULT_WIDTH;
    s_height = g_docked ? DOCKED_HEIGHT : DEFAULT_HEIGHT;
    s_isFocused = true;
    s_isFullscreenCursorVisible = false;

    // Swap chains are made at this size from now on (a later size goes through the swap chain, see Update).
    plume::SetSwitchSwapChainSize(s_width, s_height);
    ApplyModeResolutionScale(g_docked);
    g_displayResolutionEventValid = R_SUCCEEDED(appletGetDefaultDisplayResolutionChangeEvent(&g_displayResolutionEvent));

    s_renderWindow = nwindowGetDefault();
    if (s_renderWindow != nullptr)
    {
        nwindowSetDimensions(s_renderWindow, s_width, s_height);
        // No crop: the whole buffer is shown, whatever size the swap chain is made at later.
        nwindowSetCrop(s_renderWindow, 0, 0, 0, 0);
        nwindowSetSwapInterval(s_renderWindow, 1);

        uint32_t nativeWidth = 0;
        uint32_t nativeHeight = 0;
        if (nwindowGetDimensions(s_renderWindow, &nativeWidth, &nativeHeight) == 0)
        {
            s_width = static_cast<int>(nativeWidth);
            s_height = static_cast<int>(nativeHeight);
        }
    }

    return;
#else
#ifdef __linux__
    SDL_SetHint("SDL_APP_ID", "io.github.hedge_dev.unleashedrecomp");
#endif

    if (SDL_VideoInit(sdlVideoDriver) != 0 && sdlVideoDriver)
    {
        LOGFN_ERROR("Failed to initialise the SDL video driver: \"{}\". Falling back to default.", sdlVideoDriver);
        SDL_VideoInit(nullptr);
    }

    auto videoDriverName = SDL_GetCurrentVideoDriver();

    if (videoDriverName)
        LOGFN("SDL video driver: \"{}\"", videoDriverName);

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_AddEventWatch(Window_OnSDLEvent, s_pWindow);

#ifdef _WIN32
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif

    s_x = Config::WindowX;
    s_y = Config::WindowY;
    s_width = Config::WindowWidth;
    s_height = Config::WindowHeight;

    if (s_x == -1 && s_y == -1)
        s_x = s_y = SDL_WINDOWPOS_CENTERED;

    if (!IsPositionValid())
        GameWindow::ResetDimensions();

    s_pWindow = SDL_CreateWindow("Unleashed Recompiled", s_x, s_y, s_width, s_height, GetWindowFlags());

    if (IsFullscreen())
        SDL_ShowCursor(SDL_DISABLE);

    SetDisplay(Config::Monitor);
    SetIcon();
    SetTitle();

    SDL_SetWindowMinimumSize(s_pWindow, MIN_WIDTH, MIN_HEIGHT);

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(s_pWindow, &info);

#if defined(_WIN32)
    s_renderWindow = info.info.win.window;

    if (Config::DisableDWMRoundedCorners)
    {
        DWM_WINDOW_CORNER_PREFERENCE wcp = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(s_renderWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &wcp, sizeof(wcp));
    }
#elif defined(SDL_VULKAN_ENABLED)
    s_renderWindow = s_pWindow;
#elif defined(__linux__)
    s_renderWindow = { info.info.x11.display, info.info.x11.window };
#elif defined(__APPLE__)
    s_renderWindow.window = info.info.cocoa.window;
    s_renderWindow.view = SDL_Metal_GetLayer(SDL_Metal_CreateView(s_pWindow));
#else
    static_assert(false, "Unknown platform.");
#endif

    SetTitleBarColour();

    SDL_ShowWindow(s_pWindow);
#endif
}

void GameWindow::Update()
{
#if defined(__SWITCH__)
    // Docked or handheld now? A new size goes to the swap chain, which is made again at it (plume's needsResize,
    // then CheckSwapChain) and sets the window's size; the game then remakes its render targets (g_needsResize),
    // at the mode's resolution scale.
    if (g_displayResolutionEventValid && R_SUCCEEDED(eventWait(&g_displayResolutionEvent, 0)))
    {
        const bool docked = QueryDocked();
        if (docked != g_docked)
        {
            g_docked = docked;
            plume::SetSwitchSwapChainSize(docked ? DOCKED_WIDTH : DEFAULT_WIDTH, docked ? DOCKED_HEIGHT : DEFAULT_HEIGHT);
            ApplyModeResolutionScale(docked);
            VideoConfigValueChangedCallback(&Config::ResolutionScale);
        }
    }

    uint32_t nativeWidth = 0;
    uint32_t nativeHeight = 0;
    if (s_renderWindow != nullptr && nwindowGetDimensions(s_renderWindow, &nativeWidth, &nativeHeight) == 0)
    {
        s_width = static_cast<int>(nativeWidth);
        s_height = static_cast<int>(nativeHeight);
    }

    if (g_needsResize)
        s_isChangingDisplay = false;

    return;
#endif
    if (!GameWindow::IsFullscreen() && !GameWindow::IsMaximised() && !s_isChangingDisplay)
    {
        Config::WindowX = GameWindow::s_x;
        Config::WindowY = GameWindow::s_y;
        Config::WindowWidth = GameWindow::s_width;
        Config::WindowHeight = GameWindow::s_height;
    }

    if (m_isResizing)
    {
        SetTitle();
        m_isResizing = false;
    }

    if (g_needsResize)
        s_isChangingDisplay = false;
}

SDL_Surface* GameWindow::GetIconSurface(void* pIconBmp, size_t iconSize)
{
#if defined(__SWITCH__)
    (void)pIconBmp;
    (void)iconSize;
    return nullptr;
#else
    auto rw = SDL_RWFromMem(pIconBmp, iconSize);
    auto surface = SDL_LoadBMP_RW(rw, 1);

    if (!surface)
        LOGF_ERROR("Failed to load icon: {}", SDL_GetError());

    return surface;
#endif
}

void GameWindow::SetIcon(void* pIconBmp, size_t iconSize)
{
#if defined(__SWITCH__)
    (void)pIconBmp;
    (void)iconSize;
#else
    if (auto icon = GetIconSurface(pIconBmp, iconSize))
    {
        SDL_SetWindowIcon(s_pWindow, icon);
        SDL_FreeSurface(icon);
    }
#endif
}

void GameWindow::SetIcon(bool isNight)
{
    if (isNight)
    {
        SetIcon(g_game_icon_night, sizeof(g_game_icon_night));
    }
    else
    {
        SetIcon(g_game_icon, sizeof(g_game_icon));
    }
}

const char* GameWindow::GetTitle()
{
    if (Config::UseOfficialTitleOnTitleBar)
    {
        auto isSWA = Config::Language == ELanguage::Japanese;

        if (Config::UseAlternateTitle)
            isSWA = !isSWA;

        return isSWA
            ? "SONIC WORLD ADVENTURE"
            : "SONIC UNLEASHED";
    }

    return "Unleashed Recompiled";
}

void GameWindow::SetTitle(const char* title)
{
#if defined(__SWITCH__)
    (void)title;
#else
    SDL_SetWindowTitle(s_pWindow, title ? title : GetTitle());
#endif
}

void GameWindow::SetTitleBarColour()
{
#if _WIN32
    if (os::user::IsDarkTheme())
    {
        auto version = os::version::GetOSVersion();

        if (version.Major < 10 || version.Build <= 17763)
            return;

        auto flag = version.Build >= 18985
            ? DWMWA_USE_IMMERSIVE_DARK_MODE
            : 19; // DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1

        const DWORD useImmersiveDarkMode = 1;
        DwmSetWindowAttribute(s_renderWindow, flag, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode));
    }
#endif
}

bool GameWindow::IsFullscreen()
{
#if defined(__SWITCH__)
    return true;
#else
    return SDL_GetWindowFlags(s_pWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
}

bool GameWindow::SetFullscreen(bool isEnabled)
{
#if defined(__SWITCH__)
    return true;
#else
    if (isEnabled)
    {
        SDL_SetWindowFullscreen(s_pWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(s_isFullscreenCursorVisible ? SDL_ENABLE : SDL_DISABLE);
    }
    else
    {
        SDL_SetWindowFullscreen(s_pWindow, 0);
        SDL_ShowCursor(SDL_ENABLE);

        SetIcon(GameWindow::s_isIconNight);
        SetDimensions(Config::WindowWidth, Config::WindowHeight, Config::WindowX, Config::WindowY);
    }

    return isEnabled;
#endif
}
    
void GameWindow::SetFullscreenCursorVisibility(bool isVisible)
{
    s_isFullscreenCursorVisible = isVisible;

#if !defined(__SWITCH__)
    if (IsFullscreen())
    {
        SDL_ShowCursor(s_isFullscreenCursorVisible ? SDL_ENABLE : SDL_DISABLE);
    }
    else
    {
        SDL_ShowCursor(SDL_ENABLE);
    }
#endif
}

bool GameWindow::IsMaximised()
{
#if defined(__SWITCH__)
    return false;
#else
    return SDL_GetWindowFlags(s_pWindow) & SDL_WINDOW_MAXIMIZED;
#endif
}

EWindowState GameWindow::SetMaximised(bool isEnabled)
{
#if defined(__SWITCH__)
    (void)isEnabled;
    return EWindowState::Normal;
#else
    if (isEnabled)
    {
        SDL_MaximizeWindow(s_pWindow);
    }
    else
    {
        SDL_RestoreWindow(s_pWindow);
    }

    return isEnabled
        ? EWindowState::Maximised
        : EWindowState::Normal;
#endif
}

SDL_Rect GameWindow::GetDimensions()
{
    SDL_Rect rect{};

#if defined(__SWITCH__)
    rect.x = 0;
    rect.y = 0;
    rect.w = s_width;
    rect.h = s_height;
#else
    SDL_GetWindowPosition(s_pWindow, &rect.x, &rect.y);
    SDL_GetWindowSize(s_pWindow, &rect.w, &rect.h);
#endif

    return rect;
}

void GameWindow::GetSizeInPixels(int *w, int *h)
{
#if defined(__SWITCH__)
    *w = s_width;
    *h = s_height;
#else
    SDL_GetWindowSizeInPixels(s_pWindow, w, h);
#endif
}

void GameWindow::SetDimensions(int w, int h, int x, int y)
{
    s_width = w;
    s_height = h;
    s_x = x;
    s_y = y;

#if defined(__SWITCH__)
    (void)x;
    (void)y;

    // The window cannot change size while the swap chain has buffers on it: the swap chain is made again at the
    // new size instead, which sets it.
    plume::SetSwitchSwapChainSize(w, h);
#else
    SDL_SetWindowSize(s_pWindow, w, h);
    SDL_ResizeEvent(s_pWindow, w, h);

    SDL_SetWindowPosition(s_pWindow, x, y);
    SDL_MoveEvent(s_pWindow, x, y);
#endif
}

void GameWindow::ResetDimensions()
{
    s_x = SDL_WINDOWPOS_CENTERED;
    s_y = SDL_WINDOWPOS_CENTERED;
    s_width = DEFAULT_WIDTH;
    s_height = DEFAULT_HEIGHT;

    Config::WindowX = s_x;
    Config::WindowY = s_y;
    Config::WindowWidth = s_width;
    Config::WindowHeight = s_height;
}

uint32_t GameWindow::GetWindowFlags()
{
#if defined(__SWITCH__)
    return 0;
#else
    uint32_t flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    if (Config::WindowState == EWindowState::Maximised)
        flags |= SDL_WINDOW_MAXIMIZED;

    if (Config::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

#ifdef SDL_VULKAN_ENABLED
    flags |= SDL_WINDOW_VULKAN;
#endif

    return flags;
#endif
}

int GameWindow::GetDisplayCount()
{
#if defined(__SWITCH__)
    return 1;
#else
    auto result = SDL_GetNumVideoDisplays();

    if (result < 0)
    {
        LOGF_ERROR("Failed to get display count: {}", SDL_GetError());
        return 1;
    }

    return result;
#endif
}

int GameWindow::GetDisplay()
{
#if defined(__SWITCH__)
    return 0;
#else
    return SDL_GetWindowDisplayIndex(s_pWindow);
#endif
}

void GameWindow::SetDisplay(int displayIndex)
{
#if defined(__SWITCH__)
    (void)displayIndex;
    return;
#else
    if (!IsFullscreen())
        return;

    if (GetDisplay() == displayIndex)
        return;

    s_isChangingDisplay = true;

    SDL_Rect bounds;

    if (SDL_GetDisplayBounds(displayIndex, &bounds) == 0)
    {
        SetFullscreen(false);
        SetDimensions(bounds.w, bounds.h, bounds.x, bounds.y);
        SetFullscreen(true);
    }
    else
    {
        ResetDimensions();
    }
#endif
}

std::vector<SDL_DisplayMode> GameWindow::GetDisplayModes(bool ignoreInvalidModes, bool ignoreRefreshRates)
{
    auto result = std::vector<SDL_DisplayMode>();

#if defined(__SWITCH__)
    (void)ignoreInvalidModes;
    (void)ignoreRefreshRates;
    SDL_DisplayMode mode{};
    mode.w = s_width;
    mode.h = s_height;
    mode.refresh_rate = 60;
    result.push_back(mode);
    return result;
#else
    auto uniqueResolutions = std::set<std::pair<int, int>>();
    auto displayIndex = GetDisplay();
    auto modeCount = SDL_GetNumDisplayModes(displayIndex);

    if (modeCount <= 0)
        return result;

    for (int i = modeCount - 1; i >= 0; i--)
    {
        SDL_DisplayMode mode;

        if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0)
        {
            if (ignoreInvalidModes)
            {
                if (mode.w < MIN_WIDTH || mode.h < MIN_HEIGHT)
                    continue;

                SDL_DisplayMode desktopMode;

                if (SDL_GetDesktopDisplayMode(displayIndex, &desktopMode) == 0)
                {
                    if (mode.w >= desktopMode.w || mode.h >= desktopMode.h)
                        continue;
                }
            }

            if (ignoreRefreshRates)
            {
                auto res = std::make_pair(mode.w, mode.h);

                if (uniqueResolutions.find(res) == uniqueResolutions.end())
                {
                    uniqueResolutions.insert(res);
                    result.push_back(mode);
                }
            }
            else
            {
                result.push_back(mode);
            }
        }
    }

    return result;
#endif
}

int GameWindow::FindNearestDisplayMode()
{
    auto result = -1;
    auto displayModes = GetDisplayModes();
    auto currentDiff = std::numeric_limits<int>::max();

    for (int i = 0; i < displayModes.size(); i++)
    {
        auto& mode = displayModes[i];

        auto widthDiff = abs(mode.w - s_width);
        auto heightDiff = abs(mode.h - s_height);
        auto totalDiff = widthDiff + heightDiff;

        if (totalDiff < currentDiff)
        {
            currentDiff = totalDiff;
            result = i;
        }
    }

    return result;
}

bool GameWindow::IsPositionValid()
{
#if defined(__SWITCH__)
    return true;
#else
    auto displayCount = GetDisplayCount();

    for (int i = 0; i < displayCount; i++)
    {
        SDL_Rect bounds;

        if (SDL_GetDisplayBounds(i, &bounds) == 0)
        {
            auto x = s_x;
            auto y = s_y;

            // Window spans across the entire display in windowed mode, which is invalid.
            if (!Config::Fullscreen && s_width == bounds.w && s_height == bounds.h)
                return false;

            if (x == SDL_WINDOWPOS_CENTERED_DISPLAY(i))
                x = bounds.w / 2 - s_width / 2;

            if (y == SDL_WINDOWPOS_CENTERED_DISPLAY(i))
                y = bounds.h / 2 - s_height / 2;

            if (x >= bounds.x && x < bounds.x + bounds.w &&
                y >= bounds.y && y < bounds.y + bounds.h)
            {
                return true;
            }
        }
    }

    return false;
#endif
}
