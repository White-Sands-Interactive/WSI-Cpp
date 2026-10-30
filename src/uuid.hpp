#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace wsi {

inline std::string uuid_v4() {
    std::array<unsigned char, 16> bytes{};
    std::random_device device;
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : bytes) byte = static_cast<unsigned char>(dist(device));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream out;
    out << std::hex;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        const int value = bytes[i];
        if (value < 16) out << '0';
        out << value;
    }
    return out.str();
}

}  // namespace wsi
