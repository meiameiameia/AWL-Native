#include "gx/gx_dump.h"

#include <cstring>
#include <filesystem>

namespace awl::gx::dump {

namespace {
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

bool Writer::open(const std::string& path) {
    close();
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) {
        std::fprintf(stderr, "gx dump: cannot create %s\n", path.c_str());
        return false;
    }
    uint8_t head[12];
    std::memcpy(head, kMagic, 8);
    put32(head + 8, kVersion);
    if (std::fwrite(head, 1, sizeof head, f_) != sizeof head) {
        std::fprintf(stderr, "gx dump: short write to %s\n", path.c_str());
        close();
        return false;
    }
    return true;
}

void Writer::record(Rec type, const uint8_t* head, size_t headN, const uint8_t* data, size_t n) {
    if (!f_) return;
    uint8_t h[5];
    h[0] = static_cast<uint8_t>(type);
    put32(h + 1, static_cast<uint32_t>(headN + n));
    bool ok = std::fwrite(h, 1, sizeof h, f_) == sizeof h;
    if (headN) ok = ok && std::fwrite(head, 1, headN, f_) == headN;
    if (n) ok = ok && std::fwrite(data, 1, n, f_) == n;
    if (!ok) {
        std::fprintf(stderr, "gx dump: write failed, capture stopped\n");
        close();
    }
}

void Writer::frame(uint32_t retrace) {
    uint8_t b[4];
    put32(b, retrace);
    record(Rec::Frame, b, 4, nullptr, 0);
}
void Writer::fifo(const uint8_t* data, size_t n) { record(Rec::Fifo, nullptr, 0, data, n); }
void Writer::displayList(uint32_t addr, uint32_t size, const uint8_t* data) {
    uint8_t b[8];
    put32(b, addr);
    put32(b + 4, size);
    record(Rec::DisplayList, b, 8, data, size);
}
void Writer::prelude(const uint8_t* data, size_t n) { record(Rec::Prelude, nullptr, 0, data, n); }

void Writer::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

bool Reader::open(const std::string& path, std::string& err) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) {
        err = "cannot open " + path;
        return false;
    }
    uint8_t head[12];
    if (std::fread(head, 1, sizeof head, f_) != sizeof head || std::memcmp(head, kMagic, 8) != 0) {
        err = path + " is not a GX dump";
        close();
        return false;
    }
    if (get32(head + 8) != kVersion) {
        err = path + ": unsupported dump version";
        close();
        return false;
    }
    return true;
}

bool Reader::next(Record& r, std::string& err) {
    err.clear();
    uint8_t h[5];
    const size_t got = f_ ? std::fread(h, 1, sizeof h, f_) : 0;
    if (got == 0) return false;
    if (got != sizeof h) {
        err = "truncated record header";
        return false;
    }
    const uint32_t len = get32(h + 1);
    std::vector<uint8_t> body(len);
    if (len && std::fread(body.data(), 1, len, f_) != len) {
        err = "truncated record body";
        return false;
    }
    r.type = static_cast<Rec>(h[0]);
    r.addr = r.size = 0;
    r.data.clear();
    switch (r.type) {
        case Rec::Frame:
            if (len != 4) { err = "bad frame record"; return false; }
            r.addr = get32(body.data());
            break;
        case Rec::DisplayList:
            if (len < 8 || len - 8 != get32(body.data() + 4)) { err = "bad display-list record"; return false; }
            r.addr = get32(body.data());
            r.size = get32(body.data() + 4);
            r.data.assign(body.begin() + 8, body.end());
            break;
        case Rec::Fifo:
        case Rec::Prelude:
            r.data = std::move(body);
            break;
        default:
            err = "unknown record type";
            return false;
    }
    return true;
}

void Reader::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

}  // namespace awl::gx::dump
