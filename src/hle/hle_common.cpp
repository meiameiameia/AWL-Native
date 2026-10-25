#include "hle_common.h"

namespace awl::hle {

void TraceLog::open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "w");
}

void TraceLog::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

void TraceLog::line(const char* fmt, ...) {
    if (!f_) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f_, fmt, ap);
    va_end(ap);
    std::fputc('\n', f_);
}

TraceLog& trace() {
    static TraceLog t;
    return t;
}

}  // namespace awl::hle
