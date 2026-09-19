// Lua-side file IO (io.open family) + host-service stubs.
//
// Behaviour contract (see runtime_lua_file.h for why):
//
//   io.open(path, mode)   path is a GAME-relative name (backslashes accepted,
//                         leading '/' tolerated, ".." and ':' rejected).
//                         Read modes resolve the save root first and the
//                         project filesystem second, so a script always reads
//                         back what it wrote this install while still being
//                         able to read shipped text files. Write modes buffer
//                         in memory and land in the save root on flush/close
//                         (io.open itself never touches the host filesystem).
//   io.lines / f:lines    same resolution, iterator semantics of 5.1.
//   io.close / io.type    recognise the handles above and delegate to the
//                         stock functions for anything else.
//
// Host services the engine refuses to expose (shell/temp-file/ambient
// streams) keep their refusal but answer with a VALUE instead of not
// existing: a 2010-era framework calling `os.execute(...)` and comparing the
// result must not turn into "attempt to call field 'execute' (a nil value)".
// That class of crash is exactly what the block list used to cause on
// third-party games (e.g. 剑与 they 远景's `system/extend/updater.lua`).
#include "core/runtime/runtime_lua_file.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/runtime/runtime_lua.h"

namespace oa::runtime {
namespace {

// ---------------------------------------------------------------------------
// Registry keys (addresses used as light-userdata keys, the usual 5.1 idiom).
// ---------------------------------------------------------------------------
char kBridgeKey = 0;      // LuaBridge* for the running VM
char kStockCloseKey = 0;  // original io.close
char kStockTypeKey = 0;   // original io.type

const char* const kFileMetaName = "oa.file";

bool stock_stdlib() {
    const char* v = std::getenv("OA_LUA_STDLIB");
    return v && std::strcmp(v, "stock") == 0;
}

bool file_debug() {
    return std::getenv("OA_LUA_FILE_DEBUG") != nullptr;
}

LuaBridge* bridge_of(lua_State* L) {
    lua_pushlightuserdata(L, static_cast<void*>(&kBridgeKey));
    lua_rawget(L, LUA_REGISTRYINDEX);
    LuaBridge* b = static_cast<LuaBridge*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return b;
}

// ---------------------------------------------------------------------------
// Path policy. Same rules as the save domain's clean_rel_path(): separators
// normalize to '/', empty/"." segments drop, ".." and ':' (drive/stream
// syntax) are refused. A leading '/' is dropped instead of refused so a
// script that builds "/savedata/x" still lands inside the save root.
// ---------------------------------------------------------------------------
bool normalize_guest_path(const char* raw, std::string* out) {
    if (!raw || !*raw) return false;
    std::string n = raw;
    for (char& c : n) {
        if (c == '\\') c = '/';
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= n.size()) {
        const size_t slash = n.find('/', start);
        const std::string seg =
            n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? n.size() + 1 : slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == ".." || seg.find(':') != std::string::npos) return false;
        parts.push_back(seg);
    }
    if (parts.empty()) return false;
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined += '/';
        joined += parts[i];
    }
    *out = std::move(joined);
    return true;
}

// ---------------------------------------------------------------------------
// File handle.
// ---------------------------------------------------------------------------
struct LuaFile {
    std::string path;         // normalized game-relative name
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool can_read = true;
    bool can_write = false;
    /// Write-mode handles commit on close even without a write (stock io.open
    /// "wb" creates/truncates the file), read-mode handles never do.
    bool must_commit = false;
    bool dirty = false;
    bool closed = false;
};

LuaFile* as_file(lua_State* L, int idx) {
    if (!lua_getmetatable(L, idx)) return nullptr;
    luaL_getmetatable(L, kFileMetaName);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? static_cast<LuaFile*>(lua_touserdata(L, idx)) : nullptr;
}

LuaFile* check_file(lua_State* L, int idx) {
    LuaFile* f = as_file(L, idx);
    if (!f) {
        if (lua_isuserdata(L, idx)) luaL_error(L, "attempt to use a closed file");
        luaL_argerror(L, idx, "file expected");
    }
    if (f->closed) luaL_error(L, "attempt to use a closed file");
    return f;
}

LuaFile* push_file(lua_State* L) {
    auto* f = static_cast<LuaFile*>(lua_newuserdata(L, sizeof(LuaFile)));
    new (f) LuaFile();
    luaL_getmetatable(L, kFileMetaName);
    lua_setmetatable(L, -2);
    return f;
}

/// Persist the buffer to the save root when the handle can write. Returns
/// false (and logs under OA_LUA_FILE_DEBUG) when no save root is wired.
bool commit_file(lua_State* L, LuaFile* f) {
    if (!f->can_write) return true;
    if (!f->dirty && !f->must_commit) return true;
    LuaBridge* b = bridge_of(L);
    if (!b || !b->host().save_write) {
        std::fprintf(stderr, "[lua] io: no writable root, dropped %s\n", f->path.c_str());
        return false;
    }
    const bool ok = b->host().save_write(f->path, f->data);
    if (ok) {
        f->dirty = false;
        f->must_commit = false;
        if (file_debug())
            std::fprintf(stderr, "[lua] io: wrote %s (%zu bytes)\n", f->path.c_str(),
                         f->data.size());
    } else {
        std::fprintf(stderr, "[lua] io: write failed: %s\n", f->path.c_str());
    }
    return ok;
}

/// Read-mode resolution: save root first, project filesystem second.
std::optional<std::vector<uint8_t>> read_guest_file(lua_State* L, const std::string& path) {
    LuaBridge* b = bridge_of(L);
    if (!b) return std::nullopt;
    if (b->host().save_read) {
        if (auto bytes = b->host().save_read(path)) return bytes;
    }
    if (b->host().read_file) return b->host().read_file(path);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Read primitives.
// ---------------------------------------------------------------------------
// Push one read result: 1 = value pushed, 0 = nil pushed (EOF),
// -1 = error message pushed (caller raises it).
int read_one(lua_State* L, LuaFile* f, int fmt_idx) {
    const size_t size = f->data.size();
    static const char kEmpty = 0;
    const char* base =
        f->data.empty() ? &kEmpty : reinterpret_cast<const char*>(f->data.data());

    const char* fmt = "*l";
    if (fmt_idx > 0 && !lua_isnoneornil(L, fmt_idx)) {
        if (lua_type(L, fmt_idx) == LUA_TNUMBER) {
            const lua_Number want = lua_tonumber(L, fmt_idx);
            if (want < 0) {
                lua_pushstring(L, "invalid format");
                return -1;
            }
            const size_t n = static_cast<size_t>(want);
            if (n == 0) {
                if (f->pos >= size) {
                    lua_pushnil(L);
                    return 0;
                }
                lua_pushliteral(L, "");
                return 1;
            }
            const size_t avail = size - (f->pos < size ? f->pos : size);
            const size_t take = n < avail ? n : avail;
            if (take == 0) {
                lua_pushnil(L);
                return 0;
            }
            lua_pushlstring(L, base + f->pos, take);
            f->pos += take;
            return 1;
        }
        fmt = luaL_checkstring(L, fmt_idx);
    }

    if (std::strcmp(fmt, "*a") == 0 || std::strcmp(fmt, "*all") == 0) {
        const size_t take = f->pos < size ? size - f->pos : 0;
        lua_pushlstring(L, base + (f->pos < size ? f->pos : size), take);
        f->pos = size;
        return 1;
    }

    if (std::strcmp(fmt, "*l") == 0 || std::strcmp(fmt, "*L") == 0) {
        if (f->pos >= size) {
            lua_pushnil(L);
            return 0;
        }
        size_t end = f->pos;
        while (end < size && base[end] != '\n') ++end;
        size_t keep = end - f->pos;
        if (keep > 0 && base[f->pos + keep - 1] == '\r') --keep;
        const bool with_newline = fmt[1] == 'L';
        if (with_newline && end < size) ++keep;  // keep '\n' for the "*L" form
        lua_pushlstring(L, base + f->pos, keep);
        f->pos = end < size ? end + 1 : size;
        return 1;
    }

    if (std::strcmp(fmt, "*n") == 0) {
        size_t i = f->pos;
        while (i < size && std::isspace(static_cast<unsigned char>(base[i]))) ++i;
        size_t j = i;
        if (j < size && (base[j] == '-' || base[j] == '+')) ++j;
        while (j < size && std::isdigit(static_cast<unsigned char>(base[j]))) ++j;
        if (j < size && base[j] == '.') {
            ++j;
            while (j < size && std::isdigit(static_cast<unsigned char>(base[j]))) ++j;
        }
        if (j < size && (base[j] == 'e' || base[j] == 'E')) {
            size_t k = j + 1;
            if (k < size && (base[k] == '-' || base[k] == '+')) ++k;
            size_t digits = k;
            while (k < size && std::isdigit(static_cast<unsigned char>(base[k]))) ++k;
            if (k > digits) j = k;
        }
        if (j == i) {
            lua_pushnil(L);
            return 0;
        }
        const std::string token(base + i, j - i);
        lua_pushnumber(L, std::strtod(token.c_str(), nullptr));
        f->pos = j;
        return 1;
    }

    lua_pushfstring(L, "invalid format '%s'", fmt);
    return -1;
}

// ---------------------------------------------------------------------------
// Handle methods.
// ---------------------------------------------------------------------------
int l_file_read(lua_State* L) {
    LuaFile* f = check_file(L, 1);
    if (!f->can_read) return luaL_error(L, "file not opened for reading");
    const int nargs = lua_gettop(L);
    if (nargs <= 1) {
        const int r = read_one(L, f, -1);
        if (r < 0) return lua_error(L);
        return 1;
    }
    int pushed = 0;
    for (int i = 2; i <= nargs; ++i) {
        const int r = read_one(L, f, i);
        if (r < 0) return lua_error(L);
        ++pushed;
        if (r == 0) break;
    }
    return pushed;
}

int l_file_write(lua_State* L) {
    LuaFile* f = check_file(L, 1);
    if (!f->can_write) return luaL_error(L, "file not opened for writing");
    for (int i = 2; i <= lua_gettop(L); ++i) {
        size_t len = 0;
        const char* s = luaL_checklstring(L, i, &len);
        if (f->pos > f->data.size()) f->data.resize(f->pos, 0);
        if (f->pos + len > f->data.size()) f->data.resize(f->pos + len);
        if (len) std::memcpy(f->data.data() + f->pos, s, len);
        f->pos += len;
        f->dirty = true;
    }
    lua_settop(L, 1);
    return 1;  // writing returns the file, so calls can be chained
}

int l_file_seek(lua_State* L) {
    LuaFile* f = check_file(L, 1);
    const char* whence = luaL_optstring(L, 2, "cur");
    const lua_Number offset = luaL_optnumber(L, 3, 0);
    size_t base = 0;
    if (std::strcmp(whence, "set") == 0) {
        base = 0;
    } else if (std::strcmp(whence, "cur") == 0) {
        base = f->pos;
    } else if (std::strcmp(whence, "end") == 0) {
        base = f->data.size();
    } else {
        return luaL_argerror(L, 2, "invalid option");
    }
    const double target = static_cast<double>(base) + static_cast<double>(offset);
    f->pos = target < 0 ? 0 : static_cast<size_t>(target);
    lua_pushnumber(L, static_cast<lua_Number>(f->pos));
    return 1;
}

int l_file_flush(lua_State* L) {
    LuaFile* f = check_file(L, 1);
    if (!commit_file(L, f)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot write %s", f->path.c_str());
        return 2;
    }
    return 0;
}

int l_file_close_impl(lua_State* L) {
    LuaFile* f = as_file(L, 1);
    if (!f) return -1;  // caller falls back to the stock function
    if (f->closed) return luaL_error(L, "attempt to use a closed file");
    const bool ok = commit_file(L, f);
    f->closed = true;
    if (!ok) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot write %s", f->path.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_file_close(lua_State* L) {
    const int r = l_file_close_impl(L);
    if (r >= 0) return r;
    return luaL_argerror(L, 1, "file expected");
}

int l_file_gc(lua_State* L) {
    LuaFile* f = as_file(L, 1);
    if (f && !f->closed && f->can_write) {
        commit_file(L, f);
        f->closed = true;
    }
    return 0;
}

int l_file_tostring(lua_State* L) {
    LuaFile* f = as_file(L, 1);
    if (!f) {
        lua_pushliteral(L, "file (closed)");
        return 1;
    }
    lua_pushfstring(L, "file (%s)", f->path.c_str());
    return 1;
}

// ---------------------------------------------------------------------------
// Iterators (f:lines / io.lines).
// ---------------------------------------------------------------------------
int l_lines_iter(lua_State* L) {
    LuaFile* f = as_file(L, lua_upvalueindex(1));
    if (!f || f->closed) return 0;
    const int nfmt = static_cast<int>(lua_objlen(L, lua_upvalueindex(2)));
    const bool auto_close = lua_toboolean(L, lua_upvalueindex(3)) != 0;

    int pushed = 0;
    if (nfmt == 0) {
        const int r = read_one(L, f, -1);
        if (r < 0) return lua_error(L);
        pushed = 1;
    } else {
        for (int i = 1; i <= nfmt; ++i) {
            lua_rawgeti(L, lua_upvalueindex(2), i);
            const int fmt_pos = lua_gettop(L);
            const int r = read_one(L, f, fmt_pos);
            lua_remove(L, fmt_pos);
            if (r < 0) return lua_error(L);
            ++pushed;
            if (r == 0) break;
        }
    }
    if (auto_close && pushed == 1 && lua_isnil(L, -1) && !f->closed) {
        commit_file(L, f);
        f->closed = true;
    }
    return pushed;
}

/// Push an iterator closure for the file at `file_idx` and the `nfmt` format
/// arguments starting at absolute index `first_fmt`.
void push_lines_iterator(lua_State* L, int file_idx, int first_fmt, int nfmt,
                         bool auto_close) {
    lua_newtable(L);
    for (int i = 0; i < nfmt; ++i) {
        lua_pushvalue(L, first_fmt + i);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushvalue(L, file_idx);  // file userdata
    lua_pushvalue(L, -2); // formats table
    lua_pushboolean(L, auto_close ? 1 : 0);
    lua_pushcclosure(L, l_lines_iter, 3);
    lua_remove(L, -2);  // drop the formats table copy
}

int l_file_lines(lua_State* L) {
    check_file(L, 1);
    push_lines_iterator(L, 1, 2, lua_gettop(L) - 1, false);
    return 1;
}

// ---------------------------------------------------------------------------
// io.open / io.close / io.lines / io.type.
// ---------------------------------------------------------------------------
int open_guest_file(lua_State* L, const char* raw, const char* mode, bool* found) {
    std::string path;
    *found = false;
    if (!normalize_guest_path(raw, &path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: invalid path", raw ? raw : "");
        return 2;
    }
    const char first = mode && *mode ? *mode : 'r';
    const bool plus = mode && std::strchr(mode, '+') != nullptr;
    bool can_read = true;
    bool can_write = false;
    bool truncate = false;
    bool append = false;
    switch (first) {
        case 'r':
            can_read = true;
            can_write = plus;
            break;
        case 'w':
            can_read = plus;
            can_write = true;
            truncate = true;
            break;
        case 'a':
            can_read = plus;
            can_write = true;
            append = true;
            break;
        default:
            lua_pushnil(L);
            lua_pushfstring(L, "%s: invalid mode", mode ? mode : "");
            return 2;
    }

    LuaFile* f = push_file(L);
    f->path = path;
    f->can_read = can_read;
    f->can_write = can_write;
    f->must_commit = can_write;

    const bool want_existing = !truncate;
    if (want_existing || can_read) {
        if (auto bytes = read_guest_file(L, path)) {
            *found = true;
            if (!truncate) f->data = std::move(*bytes);
        }
    }
    if (can_write && append) f->pos = f->data.size();
    if (first == 'r' && !*found) {
        lua_pop(L, 1);  // drop the handle
        lua_pushnil(L);
        lua_pushfstring(L, "%s: No such file or directory", raw ? raw : path.c_str());
        return 2;
    }
    if (file_debug()) {
        std::fprintf(stderr, "[lua] io.open %s mode=%s -> %s (%zu bytes)%s\n", raw ? raw : "",
                     mode ? mode : "r", path.c_str(), f->data.size(),
                     *found ? "" : " [new]");
    }
    return 1;
}

int l_io_open(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");
    bool found = false;
    return open_guest_file(L, raw, mode, &found);
}

int l_io_close(lua_State* L) {
    const int r = l_file_close_impl(L);
    if (r >= 0) return r;
    const int nargs = lua_gettop(L);
    lua_pushlightuserdata(L, static_cast<void*>(&kStockCloseKey));
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_insert(L, 1);
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L);
}

int l_io_type(lua_State* L) {
    LuaFile* f = as_file(L, 1);
    if (f) {
        lua_pushstring(L, f->closed ? "closed file" : "file");
        return 1;
    }
    lua_pushlightuserdata(L, static_cast<void*>(&kStockTypeKey));
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_insert(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

int l_io_lines(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    bool found = false;
    const int nres = open_guest_file(L, raw, "r", &found);
    if (nres != 1) return nres;
    // handle sits at index 2 (index 1 is the file name); formats follow it.
    push_lines_iterator(L, 2, 3, lua_gettop(L) - 2, true);
    return 1;
}

// ---------------------------------------------------------------------------
// Host-service stubs: refusal with a value instead of a missing field.
// ---------------------------------------------------------------------------
int l_os_execute(lua_State* L) {
    const char* cmd = luaL_optstring(L, 1, "");
    if (file_debug()) std::fprintf(stderr, "[lua] os.execute blocked: %s\n", cmd);
    lua_pushinteger(L, 1);  // non-zero: the command did not run successfully
    return 1;
}

int l_os_remove(lua_State* L) {
    const char* p = luaL_checkstring(L, 1);
    lua_pushnil(L);
    lua_pushfstring(L, "%s: operation not permitted", p);
    lua_pushinteger(L, 1);
    return 3;
}

int l_os_rename(lua_State* L) {
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    lua_pushnil(L);
    lua_pushliteral(L, "operation not permitted");
    lua_pushinteger(L, 1);
    return 3;
}

int l_os_tmpname(lua_State* L) {
    static unsigned seq = 0;
    lua_pushfstring(L, "tmp/oa_tmp_%u", ++seq);
    return 1;
}

int l_os_setlocale(lua_State* L) {
    lua_pushnil(L);  // locale unchanged (message: "not available")
    lua_pushliteral(L, "locale change not supported");
    return 2;
}

int l_io_popen(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    lua_pushliteral(L, "popen not supported");
    return 2;
}

int l_io_tmpfile(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    lua_pushliteral(L, "tmpfile not supported");
    return 2;
}

int l_io_input(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    return 1;
}

void set_io_field(lua_State* L, const char* name, lua_CFunction fn) {
    lua_getglobal(L, LUA_IOLIBNAME);
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void set_os_field(lua_State* L, const char* name, lua_CFunction fn) {
    lua_getglobal(L, LUA_OSLIBNAME);
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

/// Save one stock function under a light-userdata registry key before it is
/// replaced, so delegating wrappers can still call it.
void stash_stock(lua_State* L, const char* lib, const char* name, char* key) {
    lua_getglobal(L, lib);
    lua_getfield(L, -1, name);
    lua_remove(L, -2);  // drop the library table, keep the function
    lua_pushlightuserdata(L, static_cast<void*>(key));
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

const luaL_Reg kFileMethods[] = {
    {"read", l_file_read},   {"write", l_file_write}, {"lines", l_file_lines},
    {"seek", l_file_seek},   {"flush", l_file_flush}, {"close", l_file_close},
    {nullptr, nullptr},
};

}  // namespace

void install_lua_file_io(lua_State* L, LuaBridge* bridge) {
    if (stock_stdlib()) return;

    lua_pushlightuserdata(L, static_cast<void*>(&kBridgeKey));
    lua_pushlightuserdata(L, static_cast<void*>(bridge));
    lua_rawset(L, LUA_REGISTRYINDEX);

    if (luaL_newmetatable(L, kFileMetaName)) {
        luaL_register(L, nullptr, kFileMethods);
        // userdata method lookup goes through __index (5.1 does not fall back
        // to the metatable itself): point it at the method table.
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_file_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_file_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);

    stash_stock(L, LUA_IOLIBNAME, "close", &kStockCloseKey);
    stash_stock(L, LUA_IOLIBNAME, "type", &kStockTypeKey);

    set_io_field(L, "open", l_io_open);
    set_io_field(L, "close", l_io_close);
    set_io_field(L, "lines", l_io_lines);
    set_io_field(L, "type", l_io_type);
    set_io_field(L, "popen", l_io_popen);
    set_io_field(L, "tmpfile", l_io_tmpfile);
    set_io_field(L, "input", l_io_input);
    set_io_field(L, "output", l_io_input);

    set_os_field(L, "execute", l_os_execute);
    set_os_field(L, "system", l_os_execute);
    set_os_field(L, "remove", l_os_remove);
    set_os_field(L, "rename", l_os_rename);
    set_os_field(L, "tmpname", l_os_tmpname);
    set_os_field(L, "setlocale", l_os_setlocale);
}

} // namespace oa::runtime
