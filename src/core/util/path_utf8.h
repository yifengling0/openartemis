#pragma once
// UTF-8 <-> native path conversion (research/129 portability half).
//
// The engine's path contract is UTF-8 bytes everywhere: PFS index entries are
// raw UTF-8, the project path arrives from the host as UTF-8, and scripts are
// decoded to UTF-8 internally. Two native API families disagree with that on
// Windows, because they decode `char*` with the process ANSI code page
// instead of UTF-8:
//
//   * std::filesystem::path(std::string) — throws "Illegal byte sequence" /
//     "No mapping for the Unicode character" for CJK directory names;
//   * the narrow CRT file APIs — errno=EILSEQ.
//
// `path(std::u8string)` is specified to decode char8_t input as UTF-8, and
// `path::u8string()` encodes back, independently of the ACP. POSIX treats
// paths as opaque bytes, so both helpers are byte-identity there.
#include <filesystem>
#include <string>
#include <string_view>

namespace oa::util {

/// Interpret UTF-8 path bytes as a native filesystem path.
std::filesystem::path native_path_from_utf8(std::string_view utf8);

/// UTF-8 bytes of a native path (for logs, VFS root strings, diagnostics).
std::string path_to_utf8(const std::filesystem::path& path);

/// Normalize bytes that came from the HOST (argv / getenv / a launcher) into
/// the engine's UTF-8 contract.
///
/// POSIX hands out raw bytes, so this is the identity. On Windows the CRT
/// answers `argv` and `getenv` in the process ANSI code page (GBK/936 on a
/// Chinese install), while internal strings are UTF-8: this decodes the ACP
/// form and re-encodes as UTF-8, and leaves byte strings that are already
/// valid UTF-8 alone (a launcher may pass UTF-8 directly).
std::string host_bytes_to_utf8(std::string_view bytes);

/// `getenv` in the UTF-8 contract (Windows: wide environment + UTF-8).
std::string env_utf8(const char* name);

} // namespace oa::util
