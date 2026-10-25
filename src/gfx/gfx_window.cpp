#include "gfx/gfx_window.h"

#include <windowsx.h>

namespace awl::gfx {

namespace {
const wchar_t* kClassName = L"AwlNativeGameWindow";

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
}  // namespace

GameWindow::~GameWindow() { destroy(); }

bool GameWindow::create(Device& dev, const WindowOptions& opts, std::string& err) {
    dev_ = &dev;
    opts_ = opts;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &GameWindow::wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { err = "RegisterClassEx failed"; return false; }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, static_cast<LONG>(opts.clientWidth), static_cast<LONG>(opts.clientHeight)};
    AdjustWindowRect(&r, style, FALSE);
    hwnd_ = CreateWindowExW(0, kClassName, widen(opts.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) { err = "CreateWindowEx failed"; return false; }
    if (!blitter_.init(dev, err) || !createBackBuffer(err)) { destroy(); return false; }
    ShowWindow(hwnd_, SW_SHOW);
    if (opts.fullscreen) toggleFullscreen();
    focused_ = GetForegroundWindow() == hwnd_;
    return true;
}

void GameWindow::destroy() {
    rtv_.Reset();
    swap_.Reset();
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
}

bool GameWindow::createBackBuffer(std::string& err) {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = dev_->factory()->CreateSwapChainForHwnd(dev_->device(), hwnd_, &d, nullptr, nullptr, swap_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = "swap chain: " + hresultText(hr); return false; }
    dev_->factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    return resizeBackBuffer(err);
}

bool GameWindow::resizeBackBuffer(std::string& err) {
    rtv_.Reset();
    dev_->context()->OMSetRenderTargets(0, nullptr, nullptr);
    HRESULT hr = swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { err = "swap chain resize: " + hresultText(hr); return false; }
    ComPtr<ID3D11Texture2D> buf;
    hr = swap_->GetBuffer(0, IID_PPV_ARGS(buf.GetAddressOf()));
    if (FAILED(hr)) { err = "swap chain buffer: " + hresultText(hr); return false; }
    hr = dev_->device()->CreateRenderTargetView(buf.Get(), nullptr, rtv_.GetAddressOf());
    if (FAILED(hr)) { err = "back buffer RTV: " + hresultText(hr); return false; }
    D3D11_TEXTURE2D_DESC td = {};
    buf->GetDesc(&td);
    bbW_ = td.Width; bbH_ = td.Height;
    needResize_ = false;
    return true;
}

void GameWindow::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { closeRequested_ = true; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void GameWindow::toggleFullscreen() {
    if (!hwnd_) return;
    if (!fullscreen_) {
        savedStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        GetWindowRect(hwnd_, &savedRect_);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, savedRect_.left, savedRect_.top, savedRect_.right - savedRect_.left, savedRect_.bottom - savedRect_.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = false;
    }
    needResize_ = true;
}

bool GameWindow::present(const Image& img, std::string& err) {
    if (!hwnd_ || !swap_) { err = "window closed"; return false; }
    if (minimized_) return true;
    if (needResize_ && !resizeBackBuffer(err)) return false;
    if (!tex_.update(*dev_, img, err)) return false;
    const Rect dest = letterbox(img.width, img.height, opts_.aspectNum, opts_.aspectDen, bbW_, bbH_);
    const bool oneToOne = dest.w == static_cast<int32_t>(img.width) && dest.h == static_cast<int32_t>(img.height);
    blitter_.blit(*dev_, rtv_.Get(), bbW_, bbH_, tex_.srv(), dest, !oneToOne);
    const HRESULT hr = swap_->Present(0, 0);
    if (FAILED(hr)) { err = "present: " + hresultText(hr); return false; }
    return true;
}

LRESULT CALLBACK GameWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GameWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<GameWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<GameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(hwnd, msg, wp, lp);
}

LRESULT GameWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool repeat = (lp & (1 << 30)) != 0;
    switch (msg) {
        case WM_CLOSE:   // the close button, Alt+F4 and the system menu all arrive here
            closeRequested_ = true;
            return 0;
        case WM_SETFOCUS:
            focused_ = true;
            if (onFocus) onFocus(true);
            return 0;
        case WM_KILLFOCUS:
            focused_ = false;
            if (onFocus) onFocus(false);
            return 0;
        case WM_SIZE:
            minimized_ = wp == SIZE_MINIMIZED;
            if (!minimized_) needResize_ = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (fullscreen_ && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   // a tap on Alt must not enter the menu loop (it would stall the host)
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { closeRequested_ = true; return 0; }
            if (wp == VK_F11) { if (!repeat) toggleFullscreen(); return 0; }
            if (onKey) onKey(static_cast<uint32_t>(wp), true);
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN) { if (!repeat) toggleFullscreen(); return 0; }
            break;   // Alt+F4 and the rest keep their system behaviour
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (onKey) onKey(static_cast<uint32_t>(wp), false);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace awl::gfx
