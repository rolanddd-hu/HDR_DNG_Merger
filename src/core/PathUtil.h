#pragma once
// UTF-8 <-> std::filesystem::path conversions. Settings, ImGui, NFD and GLFW all speak UTF-8,
// while path::string() uses the process code page, so always convert explicitly.

#include <filesystem>
#include <string>
#include <string_view>

namespace hdr {

inline std::string toUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::filesystem::path pathFromUtf8(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace hdr
