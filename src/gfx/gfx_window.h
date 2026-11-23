// The game window: a Win32 window with a flip-model swap chain. Resizable, borderless-fullscreen toggle (F11 or
// Alt+Enter), closes on Esc (after a Yes/No question), the close button and Alt+F4. Everything runs on the thread that creates it: the host's
// guest fibers share that thread, so pumping messages from a retrace hook is legal.
//
// Not exercised by the automated tests (they never open a window); verified by hand at an agreed time.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>

#include "gfx/gfx_blit.h"
#include "gfx/gfx_device.h"

namespace awl::gfx {

struct WindowOptions {
    std::string title = "Harvest Moon: A Wonderful Life (native)";
    uint32_t clientWidth = 640, clientHeight = 480;   // initial client area (the 1x size)
    bool fitLarge = true;                             // start at 2x (an exact multiple of the 640 x 480 base) when the screen has room for it
    bool fullscreen = false;
    uint32_t aspectNum = 4, aspectDen = 3;            // display aspect of the picture (letterboxed in the client area)
};

// A modal message box with no parent window (start-up failures, before or without a game window).
void showMessageBox(const std::string& title, const std::string& text);

class GameWindow {
public:
    ~GameWindow();

    bool create(Device& dev, const WindowOptions& opts, std::string& err);
    void destroy();

    // Handles every pending message; returns when the queue is empty.
    void pump();
    bool closeRequested() const { return closeRequested_; }
    void requestClose() { closeRequested_ = true; }
    bool focused() const { return focused_; }
    bool fullscreen() const { return fullscreen_; }
    bool minimized() const { return minimized_; }
    void toggleFullscreen();

    void setTitle(const std::string& title);
    // A modal message box over the window (the run stopped: show why instead of vanishing).
    void showMessage(const std::string& title, const std::string& text);
    bool confirmQuit();                                     // Esc: "Close the game?" Yes/No, default No; true = quit

    // Draws `img` into the back buffer (letterboxed, cleared to black around it, sharp-bilinear when the size is not 1:1) and presents
    // in step with the display (vsync: one refresh at the earliest, never torn).
    bool present(const Image& img, std::string& err);
    // True when the window changed (resized, restored, fullscreen toggled) and the last picture should be drawn again, e.g. while paused.
    bool needsRedraw() const { return needResize_; }
    // Refresh rate of the monitor the window is on, in Hz (0 when unknown). Re-read after moves, fullscreen toggles and display changes.
    int refreshHz() const { return refreshHz_; }

    std::function<void(uint32_t vk, bool down)> onKey;    // keys other than the window's own (Esc, F11, Alt+Enter)
    std::function<void(bool focused)> onFocus;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool createBackBuffer(std::string& err);
    bool resizeBackBuffer(std::string& err);
    void updateRefreshRate();

    Device* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    Blitter blitter_;
    ImageTexture tex_;
    uint32_t bbW_ = 0, bbH_ = 0;
    int refreshHz_ = 0;
    WindowOptions opts_;
    bool closeRequested_ = false, focused_ = false, fullscreen_ = false, minimized_ = false, needResize_ = false;
    // saved windowed state for leaving fullscreen
    LONG savedStyle_ = 0;
    RECT savedRect_ = {};
};

}  // namespace awl::gfx
