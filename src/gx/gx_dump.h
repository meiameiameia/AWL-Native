// GX command-stream dump: a file of records captured from the write-gather FIFO by dev_gx.cpp (AWL_GX_CAPTURE) and read
// by the survey tool and by tests. It holds game-derived data and therefore lives only under the ignored build/ folder.
//
// File: "AWLGXDMP", u32 version, then records [u8 type][u32 length][payload], all integers little-endian.
//   Frame        payload u32 retrace number; the following records belong to that retrace interval.
//   Fifo         raw bytes exactly as the CPU wrote them to the write-gather port (the command stream the CP receives).
//   DisplayList  payload u32 guest address, u32 size, then the bytes read from guest memory at the moment of the call.
//                The record is written BEFORE the Fifo record that completes the call command, in call order (a list that
//                calls another list is followed by the inner list's record), so a reader keeps a queue and pops it
//                whenever it meets a call command.
//   Prelude      state-setting commands (CP load, direct XF load, BP load) of the frames before the first captured frame,
//                one whole command per record, so a dump that starts later still has the register state. Indexed XF
//                loads and draws are not part of the prelude; matrices loaded from memory arrays are therefore not
//                reproduced for a dump that does not start at frame 0.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace awl::gx::dump {

constexpr char kMagic[8] = {'A', 'W', 'L', 'G', 'X', 'D', 'M', 'P'};
constexpr uint32_t kVersion = 1;

enum class Rec : uint8_t { Frame = 1, Fifo = 2, DisplayList = 3, Prelude = 4 };

class Writer {
public:
    Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() { close(); }

    bool open(const std::string& path);   // creates the parent folder; false (and a message on stderr) on failure
    bool isOpen() const { return f_ != nullptr; }
    void frame(uint32_t retrace);
    void fifo(const uint8_t* data, size_t n);
    void displayList(uint32_t addr, uint32_t size, const uint8_t* data);
    void prelude(const uint8_t* data, size_t n);
    void close();

private:
    void record(Rec type, const uint8_t* head, size_t headN, const uint8_t* data, size_t n);
    FILE* f_ = nullptr;
};

struct Record {
    Rec type = Rec::Fifo;
    uint32_t addr = 0, size = 0;      // DisplayList: guest address and size; Frame: addr = retrace number
    std::vector<uint8_t> data;        // Fifo / Prelude bytes, or the display-list bytes
};

class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader() { close(); }

    bool open(const std::string& path, std::string& err);
    bool next(Record& r, std::string& err);   // false at the end of the file; err stays empty then, or says what is wrong
    void close();

private:
    FILE* f_ = nullptr;
};

}  // namespace awl::gx::dump
