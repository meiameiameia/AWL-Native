#include "gfx/gfx_output.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace awl::gfx {

bool DiagnosticSource::produce(const FrameDesc& frame, Image& out, std::string& err) {
    if (!frame.width || !frame.height || frame.width > 4096 || frame.height > 4096) {
        err = "framebuffer size out of range: " + std::to_string(frame.width) + "x" + std::to_string(frame.height);
        return false;
    }
    drawDiagnosticFrame(out, frame.width, frame.height, frame.latchIndex);
    return true;
}

VideoOutput::~VideoOutput() {
    window_.reset();
    if (timerPeriod_) timeEndPeriod(1);
}

bool VideoOutput::init(const OutputOptions& opts, std::string& err) {
    opts_ = opts;
    if (opts_.window) opts_.pace = true;
    if (opts_.captureFrames) {
        if (opts_.captureDir.empty()) { err = "--capture-frames needs a folder"; return false; }
        std::error_code ec;
        std::filesystem::create_directories(opts_.captureDir, ec);
        if (ec) { err = "cannot create " + opts_.captureDir + ": " + ec.message(); return false; }
    }
    if (opts_.pace && !timerPeriod_) { timeBeginPeriod(1); timerPeriod_ = true; }   // 1 ms sleep granularity for the pacer
    if (opts_.window || opts_.captureFrames || opts_.bench) {
        DeviceOptions d;
        d.forceWarp = opts_.forceWarp;
        if (!device_.create(d, err)) return false;
        haveDevice_ = true;
        if (!opts_.bench) {   // the bench never presents or captures, so it needs no blitter
            if (!blitter_.init(device_, err)) return false;
            haveBlitter_ = true;
        }
    }
    if (opts_.window) {
        window_ = std::make_unique<GameWindow>();
        if (!window_->create(device_, opts_.windowOptions, err)) { window_.reset(); return false; }
        window_->onKey = [this](uint32_t vk, bool down) {
            if (vk == VK_F1) {   // F1 shows / hides the help text over the picture (a held key toggles once)
                if (down && !f1Down_) overlay_ = !overlay_;
                f1Down_ = down;
                return;
            }
            input_.onKey(vk, down);
        };
        window_->onFocus = [this](bool f) {
            if (!f) f1Down_ = false;   // a key-up that happens while unfocused is never seen
            input_.setFocus(f);
        };
        window_->setTitle(windowTitle(opts_.windowOptions.title, -1));
        std::printf("awl-host: display refresh rate %d Hz\n", window_->refreshHz());
        input_.setFocus(window_->focused());
    }
    return true;
}

bool VideoOutput::renderOffscreen(const Image& img, Image& out, std::string& err) {
    if (!haveDevice_ || !haveBlitter_) { err = "no device"; return false; }
    if (!target_.texture() || target_.width() != img.width || target_.height() != img.height)
        if (!target_.create(device_, img.width, img.height, err)) return false;
    if (!tex_.update(device_, img, err)) return false;
    const Rect all{0, 0, static_cast<int32_t>(img.width), static_cast<int32_t>(img.height)};
    blitter_.blit(device_, target_.rtv(), img.width, img.height, tex_.srv(), all, false);
    return target_.readback(device_, out, err);
}

void VideoOutput::waitUntil(int64_t deadline) {
    const int64_t slice = hostClockFrequency() / 200;   // 5 ms: keep the window responsive while waiting
    while (hostClockNow() < deadline) {
        if (window_) window_->pump();
        const int64_t now = hostClockNow();
        hostSleepUntil(deadline - now > slice ? now + slice : deadline);
    }
}

bool VideoOutput::presentImage(const Image& img, const Image* shown) {
    if (window_ && !window_->present(shown ? *shown : img, error_)) return false;
    if (captured_ < opts_.captureFrames && presented_ >= opts_.captureFrom && (presented_ - opts_.captureFrom) % std::max<uint32_t>(1, opts_.captureEvery) == 0) {
        Image readback;
        if (!renderOffscreen(img, readback, error_)) return false;
        char name[64];
        std::snprintf(name, sizeof name, "frame_%04llu.bmp", static_cast<unsigned long long>(presented_));
        if (!writeBmp((std::filesystem::path(opts_.captureDir) / name).string(), readback, error_)) return false;
        ++captured_;
    }
    ++presented_;
    source_->afterPresent();
    return true;
}

Image makePausedImage(const Image& last) {
    // The last picture, dimmed, with a big notice in the middle: the paused window must say so on its own, also in fullscreen (no title bar).
    Image p = last;
    if (p.width == 0 || p.height == 0) { p.resize(640, 448); p.fill(0, 0, 0); }
    for (uint32_t y = 0; y < p.height; ++y)
        for (uint32_t x = 0; x < p.width; ++x) {
            uint8_t* d = p.px(x, y);
            d[0] = static_cast<uint8_t>(d[0] / 2); d[1] = static_cast<uint8_t>(d[1] / 2); d[2] = static_cast<uint8_t>(d[2] / 2);
        }
    const std::vector<std::string> lines = pausedLines();
    const uint32_t k = std::max<uint32_t>(1, p.width / 640);
    const uint32_t scales[2] = {6 * k, 2 * k};
    const uint32_t totalH = 16 * (scales[0] + scales[1]);
    uint32_t y = p.height > totalH ? (p.height - totalH) / 2 : 0;
    for (size_t i = 0; i < lines.size() && i < 2; ++i) {
        const uint32_t w = textWidth(lines[i], scales[i]) + 6 * scales[i];
        drawTextPanel(p, p.width > w ? (p.width - w) / 2 : 0, y, scales[i], {lines[i]});
        y += 16 * scales[i];
    }
    return p;
}

bool VideoOutput::presentPaused() {
    const bool ok = window_->present(makePausedImage(lastFrame_), error_);
    source_->afterPresent();   // the blit used the device context: the renderer must not trust the state it set last
    return ok;
}

std::string VideoOutput::benchReport() const {
    if (!opts_.bench || !benchStartRetrace_ || benchLastRetrace_ <= benchStartRetrace_) return std::string();
    const double wall = static_cast<double>(benchLastClock_ - benchStartClock_) / static_cast<double>(hostClockFrequency());
    const double game = static_cast<double>(benchLastRetrace_ - benchStartRetrace_) * 1001.0 / 60000.0;   // one retrace = 1001/60000 s
    char line[200];
    std::snprintf(line, sizeof line, "speed: retraces %llu-%llu, game %.2f s, wall %.2f s, %.0f%% of real time", static_cast<unsigned long long>(benchStartRetrace_),
                  static_cast<unsigned long long>(benchLastRetrace_), game, wall, wall > 0 ? 100.0 * game / wall : 0.0);
    return line;
}

RetraceAction VideoOutput::onRetrace(const FrameDesc* frame) {
    ++retraces_;
    if (opts_.bench) {
        const int64_t now = hostClockNow();
        if (retraces_ == opts_.benchFrom) { benchStartRetrace_ = retraces_; benchStartClock_ = now; }
        if (benchStartRetrace_) { benchLastRetrace_ = retraces_; benchLastClock_ = now; }
        if (frame) {   // what the window has to produce for this frame (the renderer's XFB readback), without presenting it
            Image img;
            if (!source_->produce(*frame, img, error_)) { std::fprintf(stderr, "video output: %s\n", error_.c_str()); return RetraceAction::Quit; }
            ++presented_;
        }
        return RetraceAction::Continue;
    }
    if (window_) {
        window_->pump();
        if (window_->closeRequested()) return RetraceAction::Quit;
        if (!window_->focused() || window_->minimized()) {
            // Focus loss pauses the game: the guest does not run, the pacer restarts on return (no catch-up burst), input is cleared.
            pacer_.pause();
            input_.clear();
            window_->setTitle(windowTitle(opts_.windowOptions.title, -1, true));
            if (!presentPaused()) {   // the paused picture is shown once, and again whenever the window changes size or comes back from minimized
                std::fprintf(stderr, "video output: %s\n", error_.c_str());
                return RetraceAction::Quit;
            }
            while ((!window_->focused() || window_->minimized()) && !window_->closeRequested()) {
                window_->pump();
                if (!window_->minimized() && window_->needsRedraw() && !presentPaused()) {
                    std::fprintf(stderr, "video output: %s\n", error_.c_str());
                    return RetraceAction::Quit;
                }
                Sleep(20);
            }
            if (window_->closeRequested()) return RetraceAction::Quit;
            input_.setFocus(true);
            pacer_.resume(hostClockNow());
            speed_.reset();
            speedPercent_ = -1;
            window_->setTitle(windowTitle(opts_.windowOptions.title, -1));
        }
    }
    // The picture is made before the pacer waits, so the wait absorbs the work and the present happens right at the due time.
    Image img, shown;
    const bool haveFrame = frame && (window_ || opts_.captureFrames);
    if (haveFrame) {
        if (!source_->produce(*frame, img, error_)) {
            std::fprintf(stderr, "video output: %s\n", error_.c_str());
            return RetraceAction::Quit;
        }
        if (window_ && overlay_) {   // the help text goes onto a copy: captures and the renderer's own image stay clean
            shown = img;
            drawTextPanel(shown, 4, 4, std::max<uint32_t>(1, shown.width / 640), overlayLines(speedPercent_, window_->refreshHz()));
        }
    }
    if (opts_.pace) {
        const int64_t due = pacer_.onRetrace(hostClockNow());
        if (due > hostClockNow()) waitUntil(due);
    }
    if (window_ && speed_.onRetrace(hostClockNow())) {
        speedPercent_ = speed_.percent();
        window_->setTitle(windowTitle(opts_.windowOptions.title, speedPercent_));
    }
    if (haveFrame) {
        if (!presentImage(img, shown.width ? &shown : nullptr)) {
            std::fprintf(stderr, "video output: %s\n", error_.c_str());
            return RetraceAction::Quit;
        }
        if (window_) lastFrame_ = std::move(img);
    } else if (frame) {
        ++presented_;
    }
    return window_ && window_->closeRequested() ? RetraceAction::Quit : RetraceAction::Continue;
}

}  // namespace awl::gfx
