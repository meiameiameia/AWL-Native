// VideoOutput: what the host does at every guest retrace. Pumps the window, applies the focus-loss pause and the
// real-time pacing, and presents / captures the framebuffer the VI latched. The host (src/hle/host_main.cpp) owns one;
// nothing here depends on the platform runtime, so it is testable with fake frames.
#pragma once

#include <memory>
#include <string>

#include "gfx/gfx_blit.h"
#include "gfx/gfx_device.h"
#include "gfx/gfx_image.h"
#include "gfx/gfx_input.h"
#include "gfx/gfx_pacing.h"
#include "gfx/gfx_window.h"

namespace awl::gfx {

// The picture the VI shows from this retrace on (decoded from the VI registers by src/hle/dev_vi.cpp).
struct FrameDesc {
    uint32_t xfbAddr = 0;          // physical address of the top-field framebuffer
    uint32_t width = 0;            // pixels per line (VI width register)
    uint32_t height = 0;           // lines per frame (active video lines; both fields when interlaced)
    uint32_t strideBytes = 0;      // bytes between lines in memory
    bool interlaced = true;
    uint64_t latchIndex = 0;       // 1-based count of framebuffer latches
};

// Produces the picture for a latched framebuffer. The GX renderer implements this with the image its EFB copy wrote
// for the XFB address; until then DiagnosticSource shows a marker.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool produce(const FrameDesc& frame, Image& out, std::string& err) = 0;
};

class DiagnosticSource : public FrameSource {
public:
    bool produce(const FrameDesc& frame, Image& out, std::string& err) override;
};

struct OutputOptions {
    bool window = false;            // open the game window and present into it
    bool pace = false;              // real-time pacing of retraces (implied by window)
    uint32_t captureFrames = 0;     // write BMPs of the first N presented frames
    std::string captureDir;         // folder for the captures (under build/)
    bool forceWarp = false;         // software device for byte-reproducible captures on any machine
    WindowOptions windowOptions;
};

enum class RetraceAction { Continue, Quit };

class VideoOutput {
public:
    ~VideoOutput();
    bool init(const OutputOptions& opts, std::string& err);
    void setSource(FrameSource* source) { source_ = source; }

    // Called at every guest retrace, with the frame the VI latched since the previous retrace (null if none).
    RetraceAction onRetrace(const FrameDesc* frame);

    // Controller state for the PAD replacement (window mode only; null otherwise).
    WindowInput* input() { return window_ ? &input_ : nullptr; }

    uint64_t presented() const { return presented_; }
    uint64_t captured() const { return captured_; }
    uint64_t reanchors() const { return pacer_.reanchors(); }
    const std::string& error() const { return error_; }
    bool usingWarp() const { return device_.isWarp(); }
    const std::string& adapterName() const { return device_.adapterName(); }

    // One frame through the device path without the window: render `img` 1:1 into an offscreen target and read it back.
    bool renderOffscreen(const Image& img, Image& out, std::string& err);

private:
    void waitUntil(int64_t deadline);
    bool presentFrame(const FrameDesc& frame);

    OutputOptions opts_;
    Device device_;
    bool haveDevice_ = false;
    std::unique_ptr<GameWindow> window_;
    WindowInput input_;
    Blitter blitter_;
    bool haveBlitter_ = false;
    OffscreenTarget target_;
    ImageTexture tex_;
    RetracePacer pacer_{hostClockFrequency()};
    DiagnosticSource diagnostic_;
    FrameSource* source_ = &diagnostic_;
    uint64_t presented_ = 0, captured_ = 0;
    std::string error_;
    bool timerPeriod_ = false;   // timeBeginPeriod(1) is active
};

}  // namespace awl::gfx
