#define NOMINMAX
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <new>
#include <vector>

namespace awl {
namespace {
constexpr size_t kMaxHashBytes = 32 * 1024 * 1024;
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::vector<uint8_t> object;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Hash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    bool initialize() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
            return false;
        DWORD size = 0, returned = 0, digest_size = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) < 0 ||
            returned != sizeof(size) || size == 0 || size > 1024 * 1024 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size), &returned, 0) < 0 ||
            returned != sizeof(digest_size) || digest_size != 20) return false;
        object.resize(size);
        return BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    }
    bool add(const uint8_t* data, size_t size) {
        return BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    }
    bool finish(Sha1* output) {
        Sha1 next;
        if (BCryptFinishHash(hash, next.data(), static_cast<ULONG>(next.size()), 0) < 0)
            return false;
        *output = next;
        return true;
    }
};
} // namespace

bool sha1_bytes(const uint8_t* data, size_t size, Sha1* output) {
    if (!output || (!data && size) || size > kMaxHashBytes) return false;
    try {
        Hash hash;
        if (!hash.initialize()) return false;
        if (size && !hash.add(data, size)) return false;
        return hash.finish(output);
    } catch (const std::bad_alloc&) { return false; }
}

bool sha1_native_file(const char* path, Sha1* output) {
    if (!path || !output) return false;
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return false;
        const auto size = input.tellg();
        if (size < 0 || size > static_cast<std::streamoff>(kMaxHashBytes)) return false;
        input.seekg(0);
        if (!input) return false;
        Hash hash;
        if (!hash.initialize()) return false;
        std::array<uint8_t, 65536> buffer;
        size_t remaining = static_cast<size_t>(size);
        while (remaining) {
            const size_t count = std::min(remaining, buffer.size());
            if (!input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count)) ||
                !hash.add(buffer.data(), count)) return false;
            remaining -= count;
        }
        if (input.peek() != std::char_traits<char>::eof() || input.bad()) return false;
        return hash.finish(output);
    } catch (const std::bad_alloc&) { return false; }
}

DiscIdentity verify_mounted_disc_identity() {
    DiscIdentity result;
    char path[MAX_PATH];
    if (!filesystem_resolve_path("/sys/boot.bin", path, sizeof(path))) return result;
    std::ifstream boot(path, std::ios::binary);
    if (!boot.read(result.game_id.data(), result.game_id.size())) return result;
    if (result.game_id != kTargetGameId) {
        result.status = DiscIdentityStatus::WrongGameId; return result;
    }
    if (!filesystem_resolve_path("/sys/main.dol", path, sizeof(path)) ||
        !sha1_native_file(path, &result.dol_sha1)) {
        result.status = DiscIdentityStatus::DolUnreadable; return result;
    }
    result.status = result.dol_sha1 == kTargetDolSha1
        ? DiscIdentityStatus::Verified : DiscIdentityStatus::WrongDol;
    return result;
}

const char* disc_identity_status_text(DiscIdentityStatus status) {
    switch (status) {
    case DiscIdentityStatus::Verified: return "GYWEE9 and target DOL SHA1 verified";
    case DiscIdentityStatus::BootUnreadable: return "Missing/unreadable/short disc/sys/boot.bin";
    case DiscIdentityStatus::WrongGameId: return "Unsupported game ID (requires GYWEE9)";
    case DiscIdentityStatus::DolUnreadable: return "Missing/unreadable/oversized disc/sys/main.dol or hash failure";
    case DiscIdentityStatus::WrongDol: return "Unsupported DOL SHA1";
    }
    return "Unknown disc identity status";
}
} // namespace awl
