#include "gfx/gfx_output.h"

#include <windows.h>
#include <mmsystem.h>

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
    if (opts_.window || opts_.captureFrames) {
        DeviceOptions d;
        d.forceWarp = opts_.forceWarp;
        if (!device_.create(d, err)) return false;
        haveDevice_ = true;
        if (!blitter_.init(device_, err)) return false;
        haveBlitter_ = true;
    }
    if (opts_.window) {
        window_ = std::make_unique<GameWindow>();
        if (!window_->create(device_, opts_.windowOptions, err)) { window_.reset(); return false; }
        window_->onKey = [this](uint32_t vk, bool down) { input_.onKey(vk, down); };
        window_->onFocus = [this](bool f) { input_.setFocus(f); };
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

bool VideoOutput::presentFrame(const FrameDesc& frame) {
    Image img;
    if (!source_->produce(frame, img, error_)) return false;
    if (window_ && !window_->present(img, error_)) return false;
    if (captured_ < opts_.captureFrames) {
        Image readback;
        if (!renderOffscreen(img, readback, error_)) return false;
        char name[64];
        std::snprintf(name, sizeof name, "frame_%04llu.bmp", static_cast<unsigned long long>(captured_));
        if (!writeBmp((std::filesystem::path(opts_.captureDir) / name).string(), readback, error_)) return false;
        ++captured_;
    }
    ++presented_;
    return true;
}

RetraceAction VideoOutput::onRetrace(const FrameDesc* frame) {
    if (window_) {
        window_->pump();
        if (window_->closeRequested()) return RetraceAction::Quit;
        if (!window_->focused() || window_->minimized()) {
            // Focus loss pauses the game: the guest does not run, the pacer restarts on return (no catch-up burst), input is cleared.
            pacer_.pause();
            input_.clear();
            while ((!window_->focused() || window_->minimized()) && !window_->closeRequested()) {
                window_->pump();
                Sleep(20);
            }
            if (window_->closeRequested()) return RetraceAction::Quit;
            input_.setFocus(true);
            pacer_.resume(hostClockNow());
        }
    }
    if (opts_.pace) {
        const int64_t due = pacer_.onRetrace(hostClockNow());
        if (due > hostClockNow()) waitUntil(due);
    }
    if (frame && (window_ || opts_.captureFrames)) {
        if (!presentFrame(*frame)) {
            std::fprintf(stderr, "video output: %s\n", error_.c_str());
            return RetraceAction::Quit;
        }
    } else if (frame) {
        ++presented_;
    }
    return window_ && window_->closeRequested() ? RetraceAction::Quit : RetraceAction::Continue;
}

}  // namespace awl::gfx
