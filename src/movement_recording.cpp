#define NOMINMAX
#include "awl/movement_recording.h"
#include "awl/startup_cli.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>
#include <utility>

namespace awl {
namespace {
constexpr uint8_t kMagic[8] = {'A','W','L','T','I','C','K','1'};
constexpr size_t kHeaderSize = 104;
constexpr size_t kTickSize = 132;
constexpr size_t kMaxFileSize = kHeaderSize + kTickSize * kMaxMovementRecordingTicks;
uint32_t float_bits(float value) {
    uint32_t bits;
    static_assert(sizeof(value) == sizeof(bits), "Requires 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
float bits_float(uint32_t value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
void append_word(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}
uint32_t read_word(const uint8_t*& cursor) {
    const uint32_t value = uint32_t(cursor[0]) | (uint32_t(cursor[1]) << 8) |
        (uint32_t(cursor[2]) << 16) | (uint32_t(cursor[3]) << 24);
    cursor += 4;
    return value;
}
bool finite_state(const MovementStateWords& state) {
    for (size_t i = 0; i < 10; ++i) if (!std::isfinite(bits_float(state[i]))) return false;
    return true;
}
bool valid_recording(const MovementRecording& recording) {
    if (recording.ticks.empty() || recording.ticks.size() > kMaxMovementRecordingTicks)
        return false;
    for (float value : recording.spawn) if (!std::isfinite(value)) return false;
    for (const auto& tick : recording.ticks)
        if ((tick.input.buttons & ~uint16_t(0x1f7f)) || !finite_state(tick.state)) return false;
    return true;
}
} // namespace

MovementStateWords capture_movement_state(const WorldMapMovementRuntime& runtime,
                                          const WorldMapMovementRuntimeStep& step) {
    MovementStateWords state;
    size_t index = 0;
    for (float value : runtime.position()) state[index++] = float_bits(value);
    const auto& steering = runtime.steering();
    for (float value : {steering.direction_x, steering.direction_z, steering.facing_x,
        steering.facing_z, steering.target_speed, steering.current_speed, steering.intensity})
        state[index++] = float_bits(value);
    const auto& pad = runtime.pad();
    for (uint32_t value : {pad.current, pad.previous, pad.pressed, pad.repeated, pad.released})
        state[index++] = value;
    for (int8_t value : {pad.stick_x, pad.stick_y, pad.substick_x, pad.substick_y})
        state[index++] = static_cast<uint8_t>(value);
    state[index++] = pad.trigger_l;
    state[index++] = pad.trigger_r;
    state[index++] = step.scene.next_bucket;
    state[index++] = step.movement.collision.resolver_contact_bits;
    return state;
}

const char* movement_state_word_name(size_t index) {
    constexpr const char* names[] = {"position.x", "position.y", "position.z",
        "steering.direction_x", "steering.direction_z", "steering.facing_x",
        "steering.facing_z", "steering.target_speed", "steering.current_speed",
        "steering.intensity", "PAD.current", "PAD.previous", "PAD.pressed",
        "PAD.repeated", "PAD.released", "PAD.stick_x", "PAD.stick_y",
        "PAD.substick_x", "PAD.substick_y", "PAD.trigger_l", "PAD.trigger_r",
        "scene.bucket", "contact.bits"};
    static_assert(sizeof(names) / sizeof(names[0]) == kMovementStateWords, "State schema mismatch");
    return index < kMovementStateWords ? names[index] : "unknown";
}

bool encode_movement_recording(const MovementRecording& recording, std::vector<uint8_t>* output) {
    if (!output || !valid_recording(recording)) return false;
    try {
        std::vector<uint8_t> bytes;
        bytes.reserve(kHeaderSize + recording.ticks.size() * kTickSize);
        bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
        append_word(bytes, 0); // Seam profile.
        append_word(bytes, 30); append_word(bytes, 1);
        bytes.insert(bytes.end(), kTargetGameId.begin(), kTargetGameId.end());
        bytes.insert(bytes.end(), 2, 0);
        for (const auto& hash : {kTargetDolSha1, recording.terrain_sha1, recording.static_sha1})
            bytes.insert(bytes.end(), hash.begin(), hash.end());
        for (float value : recording.spawn) append_word(bytes, float_bits(value));
        append_word(bytes, static_cast<uint32_t>(recording.ticks.size()));
        uint32_t sequence = 0;
        for (const auto& tick : recording.ticks) {
            append_word(bytes, ++sequence);
            append_word(bytes, tick.reset_before ? 1u : 0u);
            append_word(bytes, tick.input.connected ? 1u : 0u);
            append_word(bytes, tick.input.buttons);
            for (int8_t value : {tick.input.stick_x, tick.input.stick_y,
                                  tick.input.substick_x, tick.input.substick_y})
                append_word(bytes, static_cast<uint8_t>(value));
            append_word(bytes, tick.input.trigger_l); append_word(bytes, tick.input.trigger_r);
            for (uint32_t word : tick.state) append_word(bytes, word);
        }
        if (bytes.size() != kHeaderSize + recording.ticks.size() * kTickSize) return false;
        *output = std::move(bytes);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool decode_movement_recording(const uint8_t* data, size_t size, MovementRecording* output) {
    if (!data || !output || size < kHeaderSize || size > kMaxFileSize ||
        std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return false;
    const uint8_t* cursor = data + 8;
    if (read_word(cursor) != 0 || read_word(cursor) != 30 || read_word(cursor) != 1 ||
        std::memcmp(cursor, kTargetGameId.data(), 6) != 0 || cursor[6] || cursor[7]) return false;
    cursor += 8;
    if (std::memcmp(cursor, kTargetDolSha1.data(), 20) != 0) return false;
    cursor += 20;
    try {
        MovementRecording next;
        std::memcpy(next.terrain_sha1.data(), cursor, 20); cursor += 20;
        std::memcpy(next.static_sha1.data(), cursor, 20); cursor += 20;
        for (float& value : next.spawn) value = bits_float(read_word(cursor));
        const uint32_t count = read_word(cursor);
        if (!count || count > kMaxMovementRecordingTicks || size != kHeaderSize + size_t(count) * kTickSize)
            return false;
        next.ticks.reserve(count);
        for (uint32_t sequence = 1; sequence <= count; ++sequence) {
            MovementRecordedTick tick;
            if (read_word(cursor) != sequence) return false;
            const uint32_t reset = read_word(cursor), connected = read_word(cursor);
            const uint32_t buttons = read_word(cursor);
            if (reset > 1 || connected > 1 || buttons > 0xffff || (buttons & ~0x1f7fu)) return false;
            tick.reset_before = reset != 0;
            tick.input.connected = connected != 0;
            tick.input.buttons = static_cast<uint16_t>(buttons);
            for (int8_t* axis : {&tick.input.stick_x, &tick.input.stick_y,
                                  &tick.input.substick_x, &tick.input.substick_y}) {
                const uint32_t value = read_word(cursor);
                if (value > 255) return false;
                const int32_t signed_value = value < 128 ? static_cast<int32_t>(value)
                                                        : static_cast<int32_t>(value) - 256;
                *axis = static_cast<int8_t>(signed_value);
            }
            const uint32_t left = read_word(cursor), right = read_word(cursor);
            if (left > 255 || right > 255) return false;
            tick.input.trigger_l = static_cast<uint8_t>(left);
            tick.input.trigger_r = static_cast<uint8_t>(right);
            for (auto& word : tick.state) word = read_word(cursor);
            if (!finite_state(tick.state)) return false;
            next.ticks.push_back(tick);
        }
        if (!valid_recording(next)) return false;
        *output = std::move(next);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool load_movement_recording(const char* path, MovementRecording* output) {
    if (!path || !output) return false;
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto size = file.tellg();
        if (size < static_cast<std::streamoff>(kHeaderSize) ||
            size > static_cast<std::streamoff>(kMaxFileSize)) return false;
        file.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            file.peek() != std::char_traits<char>::eof() || file.bad()) return false;
        return decode_movement_recording(bytes.data(), bytes.size(), output);
    } catch (const std::bad_alloc&) { return false; }
}

bool save_movement_recording(const char* path, const MovementRecording& recording) {
    try {
        if (!path || !valid_movement_recording_path(path)) return false;
        const DWORD attributes = GetFileAttributesA("build");
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        std::vector<uint8_t> bytes;
        if (!encode_movement_recording(recording, &bytes)) return false;
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool success = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file);
        if (!CloseHandle(file)) success = false;
        if (!success) DeleteFileA(path); // Only this call's newly created file.
        return success;
    } catch (const std::bad_alloc&) { return false; }
}
} // namespace awl
