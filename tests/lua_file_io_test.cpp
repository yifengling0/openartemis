// Lua file IO: io.open maps the GAME namespace onto the save root (writable)
// and the project filesystem (read-only), so ハミダシ系 boot scripts that
// persist savedata/*.dat through plain Lua IO work without the game
// directory being the process working directory. Also pins the host-service
// stubs (os.execute family) that used to be missing fields.
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/runtime/runtime_lua.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::LuaBridge;
using oa::runtime::LuaHost;

struct Store {
    std::map<std::string, std::vector<uint8_t>> writable;  // save root
    std::map<std::string, std::vector<uint8_t>> assets;    // project filesystem
    int writes = 0;

    LuaHost host() {
        LuaHost h;
        h.save_write = [this](const std::string& path, const std::vector<uint8_t>& data) {
            writable[path] = data;
            ++writes;
            return true;
        };
        h.save_read = [this](const std::string& path) -> std::optional<std::vector<uint8_t>> {
            const auto it = writable.find(path);
            if (it == writable.end()) return std::nullopt;
            return it->second;
        };
        h.read_file = [this](const std::string& path) -> std::optional<std::vector<uint8_t>> {
            const auto it = assets.find(path);
            if (it == assets.end()) return std::nullopt;
            return it->second;
        };
        h.is_file_exists = [this](const std::string& path) {
            return assets.count(path) != 0 || writable.count(path) != 0;
        };
        return h;
    }

    std::string text(const std::string& path) const {
        const auto it = writable.find(path);
        if (it == writable.end()) return "<missing>";
        return std::string(it->second.begin(), it->second.end());
    }
};

void test_write_then_read_back() {
    Store store;
    LuaBridge eng(store.host());
    eng.run_code(R"LUA(
local f = io.open("savedata/system.dat", "wb")
assert(f, "io.open wb must succeed")
assert(io.type(f) == "file", "io.type(file)")
f:write("hello", " ", 42)
f:close()
assert(io.type(f) == "closed file", "io.type after close")

local g = io.open("savedata/system.dat", "rb")
assert(g, "read back what this session wrote")
assert(g:read("*all") == "hello 42", "content round-trip")
g:close()

-- backslash separators and a leading slash stay inside the namespace
local h = io.open("\\savedata\\extra.dat", "wb")
assert(h, "backslash path")
h:write("x")
h:close()
)LUA", "io_write");
    check(store.text("savedata/system.dat") == "hello 42", "wb lands in the save root");
    check(store.writes == 2, "two commits");
}

void test_read_from_assets_and_lines() {
    Store store;
    store.assets["system/text.txt"] = {'l', '1', '\n', 'l', '2', '\n'};
    LuaBridge eng(store.host());
    eng.run_code(R"LUA(
local f = io.open("system/text.txt", "r")
assert(f, "asset read falls through to the project filesystem")
assert(f:read("*l") == "l1", "read *l")
assert(f:read("*a") == "l2\n", "read *a")
f:close()

local seen = {}
for line in io.lines("system/text.txt") do seen[#seen + 1] = line end
assert(#seen == 2 and seen[1] == "l1" and seen[2] == "l2", "io.lines")

local missing = io.open("system/nope.txt", "r")
assert(missing == nil, "missing asset read -> nil")
)LUA", "io_read");
    check(store.writes == 0, "reads never write");
}

void test_path_policy_and_seek() {
    Store store;
    LuaBridge eng(store.host());
    eng.run_code(R"LUA(
assert(io.open("../escape.dat", "wb") == nil, ".. refused")
assert(io.open("C:/tmp/escape.dat", "wb") == nil, "drive path refused")

local f = io.open("tmp/seek.dat", "wb")
assert(f)
f:write("abcd")
assert(f:seek("set", 1) == 1, "seek set")
f:write("X")
assert(f:seek("end", 0) == 4, "seek end")
f:close()

local r = io.open("tmp/seek.dat", "rb")
assert(r:read("*a") == "aXcd", "overwrite at position")
r:close()
)LUA", "io_policy");
    check(store.writable.count("../escape.dat") == 0, "no escape write");
    check(store.writable.count("C:/tmp/escape.dat") == 0, "no drive write");
    check(store.text("tmp/seek.dat") == "aXcd", "seeked write");
}

void test_host_service_stubs() {
    Store store;
    LuaBridge eng(store.host());
    eng.run_code(R"LUA(
-- The 2010-era frameworks call these and compare the result; a missing
-- field used to abort the whole boot (剑与 they's updater.lua).
assert(os.execute("noop") == 1, "os.execute answers a failure code")
assert(os.system("noop") == 1, "os.system answers a failure code")
assert(os.remove("x") == nil, "os.remove refuses")
assert(os.rename("x", "y") == nil, "os.rename refuses")
assert(type(os.tmpname()) == "string", "os.tmpname hands out a sandbox path")
assert(io.popen("x") == nil, "io.popen refuses")
assert(io.tmpfile() == nil, "io.tmpfile refuses")
)LUA", "io_host_services");
}

}  // namespace

int main() {
    test_write_then_read_back();
    test_read_from_assets_and_lines();
    test_path_policy_and_seek();
    test_host_service_stubs();
    if (failures) {
        std::fprintf(stderr, "lua_file_io_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("lua_file_io_test: ok\n");
    return 0;
}
