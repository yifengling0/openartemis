#include "core/util/path_utf8.h"

#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace oa::util {
namespace {

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* w, int len) {
    if (!w || len <= 0) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(size_t(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    return out;
}

bool is_valid_utf8(std::string_view s) {
    if (s.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()),
                               nullptr, 0) > 0;
}
#endif

} // namespace

std::filesystem::path native_path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(u8);
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
    return path.native();
#endif
}

std::string host_bytes_to_utf8(std::string_view bytes) {
#ifdef _WIN32
    if (bytes.empty()) return {};
    if (is_valid_utf8(bytes)) return std::string(bytes);
    const int wlen = MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()),
                                         nullptr, 0);
    if (wlen <= 0) return std::string(bytes);
    std::wstring w(size_t(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), w.data(), wlen);
    return wide_to_utf8(w.data(), wlen);
#else
    return std::string(bytes);
#endif
}

std::string env_utf8(const char* name) {
    if (!name || !*name) return {};
#ifdef _WIN32
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(
        std::wstring(name, name + std::strlen(name)).c_str(), buf,
        DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    return wide_to_utf8(buf, int(n));
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

} // namespace oa::util
