#include "persistent_id.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>

namespace {

std::mt19937_64& randomGenerator() {
    static std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(),
                           device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    return generator;
}

std::mutex& randomGeneratorMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

namespace persistent_id {

std::string generate() {
    std::array<std::uint8_t, 16> bytes{};
    {
        std::lock_guard<std::mutex> lock(randomGeneratorMutex());
        std::mt19937_64& generator = randomGenerator();
        for (std::size_t offset = 0; offset < bytes.size();
             offset += sizeof(std::uint64_t)) {
            const std::uint64_t word = generator();
            for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
                bytes[offset + byte] = static_cast<std::uint8_t>(
                    (word >> (byte * 8)) & 0xffu);
            }
        }
    }

    // Mark the value as a UUID version 4 with the RFC 4122 variant while
    // retaining the random bits used for the identity itself.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fu) | 0x80u);

    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(hexadecimal[(bytes[index] >> 4) & 0x0fu]);
        result.push_back(hexadecimal[bytes[index] & 0x0fu]);
    }
    return result;
}

}  // namespace persistent_id
