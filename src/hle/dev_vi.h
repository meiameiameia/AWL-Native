// Video interface: the hook through which the host observes retraces and the framebuffer the VI shows.
// Nothing here changes guest-visible behaviour or the boot trace; with no hook installed the model is unchanged.
#pragma once

#include <cstdint>
#include <functional>

namespace awl::hle {

// The picture the VI shows from this field on, decoded from the VI registers (TFBL, VTR, DCR, picture configuration,
// framebuffer width). Layout evidence: docs/research/gfx-dx11.md, section 1.
struct ViFrameInfo {
    uint32_t xfbAddr = 0;        // physical byte address of the top-field framebuffer (TFBL, page-offset bit applied)
    uint32_t width = 0;          // pixels per line
    uint32_t height = 0;         // lines in the frame: 2 x active video lines when interlaced
    uint32_t strideBytes = 0;    // bytes per line in memory
    bool interlaced = true;
    uint64_t latchIndex = 0;     // 1-based count of framebuffer latches (Platform::counters.fbLatches)
};

// Called by the VI model at every retrace (start of a field), on the guest thread, before the retrace is counted.
// `frame` is non-null when the framebuffer registers changed since the previous retrace and the display is enabled.
// The hook may call Platform::requestExit. Install before Platform::run; pass an empty function to remove.
using ViRetraceHook = std::function<void(const ViFrameInfo* frame)>;
void setViRetraceHook(ViRetraceHook hook);

}  // namespace awl::hle
