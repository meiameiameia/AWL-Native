#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace awl {
using Sha1 = std::array<uint8_t, 20>;
inline constexpr std::array<char, 6> kTargetGameId{'G','Y','W','E','E','9'};
inline constexpr Sha1 kTargetDolSha1{
    0x1c,0xcf,0xd9,0xdf,0xb5,0xc2,0x50,0xc2,0xf4,0x5c,
    0x70,0xc7,0x4c,0xc4,0x5e,0x5d,0x88,0xd2,0x23,0x74};
// Windows CNG; native identity check, not a translated game operation.
// At most 32 MiB per input. Failures preserve the output.
[[nodiscard]] bool sha1_bytes(const uint8_t* data, size_t size, Sha1* output);
[[nodiscard]] bool sha1_native_file(const char* path, Sha1* output);
enum class DiscIdentityStatus { Verified, BootUnreadable, WrongGameId, DolUnreadable, WrongDol };
struct DiscIdentity {
    DiscIdentityStatus status = DiscIdentityStatus::BootUnreadable;
    std::array<char, 6> game_id{};
    Sha1 dol_sha1{};
};
// Read only, through the existing mount; no memory arena or window required.
DiscIdentity verify_mounted_disc_identity();
const char* disc_identity_status_text(DiscIdentityStatus status);
} // namespace awl
