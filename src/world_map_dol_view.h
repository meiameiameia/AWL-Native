#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace awl::detail {
inline constexpr size_t max_dol_size = 32 * 1024 * 1024;
inline uint32_t dol_word(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
// File/address bounds policy shared by the two immutable startup providers.
// A mapped extent does not establish table length or target identity.
class DolView {
public:
    bool initialize(const uint8_t* data, size_t size) {
        if (!data || size < 0x100 || size > max_dol_size) return false;
        data_ = data;
        for (size_t i = 0; i < sections_.size(); ++i) {
            const Section section{dol_word(data + i * 4), dol_word(data + 0x48 + i * 4), dol_word(data + 0x90 + i * 4)};
            if (section.size == 0) continue;
            if (section.offset < 0x100 || section.offset > size || section.size > size - section.offset ||
                uint64_t(section.address) + section.size > uint64_t(UINT32_MAX) + 1) return false;
            for (size_t j = 0; j < i; ++j) {
                const auto& prior = sections_[j];
                if (prior.size == 0) continue;
                if ((uint64_t(section.address) < uint64_t(prior.address) + prior.size &&
                     uint64_t(prior.address) < uint64_t(section.address) + section.size) ||
                    (uint64_t(section.offset) < uint64_t(prior.offset) + prior.size &&
                     uint64_t(prior.offset) < uint64_t(section.offset) + section.size)) return false;
            }
            sections_[i] = section;
        }
        return true;
    }
    const uint8_t* read(uint32_t address, size_t size) const {
        for (const auto& section : sections_) {
            if (section.size != 0 && address >= section.address &&
                uint64_t(address) - section.address + size <= section.size)
                return data_ + section.offset + (address - section.address);
        }
        return nullptr;
    }
private:
    struct Section { uint32_t offset = 0, address = 0, size = 0; };
    const uint8_t* data_ = nullptr;
    std::array<Section,18> sections_{};
};
} // namespace awl::detail
