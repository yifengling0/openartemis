#include "core/fs/compat_config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "core/fs/fs.h"

namespace oa::fs {
namespace {

// ---------------------------------------------------------------------------
// Minimal flat-JSON reader.
//
// The manifest is a small hand-written config file; the engine only needs
// top-level scalars plus the one nested `inputGate` object. This reader
// therefore walks the text, tracks container depth, and reports the scalar
// pairs it recognises — anything else (arrays, unknown nested objects,
// numbers) is skipped structurally. Malformed input never throws: a manifest
// is an optional convenience, not project data the engine trusts.
// ---------------------------------------------------------------------------

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

struct Pair {
    std::string key;
    std::string value;   // string content, or "true"/"false"/number text
    bool is_string = false;
    bool is_bool = false;
    bool bool_value = false;
    int depth = 0;       // container depth of the pair (0 = top level)
};

class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    std::vector<Pair> pairs() {
        std::vector<Pair> out;
        for (;;) {
            skip_ws();
            if (at_end()) break;
            const char c = peek();
            if (c == '{') {
                ++p_;
                // The outermost object IS the manifest: its keys are depth 0.
                if (seen_root_) ++depth_;
                else seen_root_ = true;
                continue;
            }
            if (c == '}') {
                ++p_;
                if (depth_ > 0) --depth_;
                continue;
            }
            if (c == '[') {
                skip_container('[', ']');
                continue;
            }
            if (c == ',') {
                ++p_;
                continue;
            }
            if (c == '"') {
                const std::string key = read_string();
                skip_ws();
                if (!at_end() && peek() == ':') ++p_;
                skip_ws();
                Pair pair;
                pair.key = key;
                pair.depth = depth_;
                if (!at_end() && peek() == '"') {
                    pair.value = read_string();
                    pair.is_string = true;
                } else if (match_literal("true")) {
                    pair.value = "true";
                    pair.is_bool = true;
                    pair.bool_value = true;
                } else if (match_literal("false")) {
                    pair.value = "false";
                    pair.is_bool = true;
                    pair.bool_value = false;
                } else if (!at_end() && peek() == '{') {
                    // Enter the nested object so `inputGate.keyboard` style
                    // pairs are reported one level down.
                    ++p_;
                    ++depth_;
                    continue;
                } else if (!at_end() && peek() == '[') {
                    // Arrays carry no engine-visible scalars: skip whole.
                    skip_container('[', ']');
                    continue;
                } else {
                    pair.value = read_scalar();
                }
                out.push_back(std::move(pair));
                continue;
            }
            // Unknown leading byte: consume one byte and keep going so a
            // hand-edited file cannot wedge the reader.
            ++p_;
        }
        return out;
    }

private:
    bool at_end() const { return p_ >= s_.size(); }

    char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }

    void skip_ws() {
        while (!at_end() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }

    std::string read_string() {
        std::string out;
        if (at_end() || s_[p_] != '"') return out;
        ++p_;
        while (!at_end()) {
            const char c = s_[p_++];
            if (c == '"') break;
            if (c == '\\' && !at_end()) {
                const char esc = s_[p_++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'u': {
                        // \uXXXX: keep BMP characters as UTF-8 (the manifests
                        // are ASCII in practice; this covers paths/labels).
                        if (p_ + 4 <= s_.size()) {
                            unsigned code = 0;
                            bool ok = true;
                            for (int i = 0; i < 4; ++i) {
                                const char h = s_[p_ + i];
                                code <<= 4;
                                if (h >= '0' && h <= '9') code |= unsigned(h - '0');
                                else if (h >= 'a' && h <= 'f') code |= unsigned(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') code |= unsigned(h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok) {
                                p_ += 4;
                                if (code < 0x80) {
                                    out.push_back(char(code));
                                } else if (code < 0x800) {
                                    out.push_back(char(0xC0 | (code >> 6)));
                                    out.push_back(char(0x80 | (code & 0x3F)));
                                } else {
                                    out.push_back(char(0xE0 | (code >> 12)));
                                    out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                                    out.push_back(char(0x80 | (code & 0x3F)));
                                }
                            }
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    bool match_literal(const char* lit) {
        const size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(p_, n, lit) != 0) return false;
        p_ += n;
        return true;
    }

    std::string read_scalar() {
        const size_t start = p_;
        while (!at_end()) {
            const char c = s_[p_];
            if (c == ',' || c == '}' || c == ']' ||
                std::isspace(static_cast<unsigned char>(c)))
                break;
            ++p_;
        }
        return std::string(s_.substr(start, p_ - start));
    }

    /// Skip a nested container token by token (strings honoured).
    void skip_container(char open, char close) {
        if (at_end() || s_[p_] != open) return;
        ++p_;
        int depth = 1;
        while (!at_end() && depth > 0) {
            const char c = s_[p_];
            if (c == '"') {
                (void)read_string();
                continue;
            }
            if (c == open) ++depth;
            else if (c == close) --depth;
            ++p_;
        }
    }

    std::string_view s_;
    size_t p_ = 0;
    int depth_ = 0;
    bool seen_root_ = false;
};

bool key_is(const std::string& key, const char* a) { return key == a; }

/// Accept both engine snake_case and the host's camelCase spelling.
bool key_is_any(const std::string& key, const char* a, const char* b) {
    return key == a || (b && key == b);
}

}  // namespace

CompatConfig parse_compat_config(const std::string& text, const std::string& name) {
    CompatConfig out;
    out.source = name;
    Reader reader(text);
    for (const Pair& p : reader.pairs()) {
        if (p.depth == 0) {
            if (key_is_any(p.key, "platform", "reportedOs") ||
                key_is(p.key, "reported_os")) {
                if (p.is_string && !p.value.empty()) {
                    out.platform = p.value;
                    out.has_platform = true;
                }
            } else if (key_is_any(p.key, "charset", "scriptCharset")) {
                if (p.is_string && !p.value.empty()) {
                    out.charset = p.value;
                    out.has_charset = true;
                }
            } else if (key_is_any(p.key, "font_override", "fontOverride")) {
                if (p.is_string && !p.value.empty()) {
                    out.font_override = p.value;
                    out.has_font_override = true;
                }
            }
            continue;
        }
        if (p.depth == 1 && key_is(p.key, "keyboard") && p.is_bool) {
            out.gate_keyboard = p.bool_value;
            out.has_input_gate = true;
        } else if (p.depth == 1 && key_is(p.key, "wheelToKeys") && p.is_bool) {
            out.gate_wheel = p.bool_value;
            out.has_input_gate = true;
        }
    }
    return out;
}

CompatConfig load_compat_config(const IFileSystem& fs) {
    static const char* kCandidates[] = {"oa_compat.json", "art3m1s.json"};
    for (const char* name : kCandidates) {
        const auto bytes = fs.read(name);
        if (!bytes) continue;
        const std::string text(bytes->begin(), bytes->end());
        CompatConfig cfg = parse_compat_config(text, name);
        if (cfg.source.empty()) cfg.source = name;
        std::printf("[app] compat manifest: %s%s\n", name,
                    cfg.any() ? "" : " (no engine-visible fields)");
        return cfg;
    }
    return CompatConfig{};
}

} // namespace oa::fs
