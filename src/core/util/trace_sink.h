// M1 结构化 trace（COMPATLAB plan §9）：JSONL 事件流。
//
// OA_TRACE_JSONL=<path> 开启（main 启动时 configure 一次）；未配置时
// enabled()==false，发射点零开销（一次原子读）。行格式：
//   {"seq":N,"ts_ms":MS,"domain":"script","op":"tag",...fields}
// 值做 JSON 转义；UTF-8 原样通过。trace 产物落在本地（workspace_temp），
// 不入库——计划 §6/§9 的资产红线由发射点自律（正文只记字数，不记内容）。
//
// 已挂域：script（标签执行）/ text（正文入域）/ asset（纹理/媒体）/ host（摘要）。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace oa::util {

class Trace {
public:
    static Trace& instance() {
        static Trace t;
        return t;
    }

    /// main 启动时调用一次；path 为空/未设 = 关闭。
    void configure(const char* path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (out_) {
            std::fclose(out_);
            out_ = nullptr;
        }
        enabled_ = false;
        if (!path || !*path) return;
        out_ = std::fopen(path, "wb");
        enabled_ = out_ != nullptr;
    }

    bool enabled() const { return enabled_; }

    /// fields = {k1, v1, k2, v2, ...}（成对；v 为字符串，含数字先 to_string）。
    void event(std::string_view domain, std::string_view op, std::uint64_t ts_ms,
               std::initializer_list<std::pair<std::string_view, std::string>> fields) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!out_) return;
        std::string line = "{\"seq\":" + std::to_string(seq_++) +
                           ",\"ts_ms\":" + std::to_string(ts_ms) +
                           ",\"domain\":\"" + json_escape(domain) +
                           "\",\"op\":\"" + json_escape(op) + "\"";
        for (const auto& [k, v] : fields) {
            line += ",\"" + json_escape(k) + "\":" + json_value(v);
        }
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), out_);
        std::fflush(out_);  // 崩溃现场保留最后事件（watchdog 依赖）
    }

private:
    static std::string json_escape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        return out;
    }
    /// 纯数字/布尔直出 JSON 值，其余加引号（调用面省 to_string）。
    /// 数字判定必须严格：至多一个小数点（"1.90.0" 这类层 id 不是数字）。
    static std::string json_value(const std::string& v) {
        if (v == "true" || v == "false" || v == "null") return v;
        bool numeric = !v.empty();
        int dots = 0;
        for (const char c : v) {
            if (c == '.') {
                if (++dots > 1) { numeric = false; break; }
                continue;
            }
            if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == 'e' ||
                  c == 'E')) {
                numeric = false;
                break;
            }
        }
        return numeric ? v : "\"" + json_escape(v) + "\"";
    }

    std::mutex mu_;
    std::FILE* out_ = nullptr;
    std::atomic<std::uint64_t> seq_{0};
    bool enabled_ = false;  // mu_ 下读写；enabled() 快路径读（configure 仅启动期）
};

/// 便捷发射（enabled 时才构造 fields；调用面用 if 包裹避免无谓构造）。
inline void trace_event(std::string_view domain, std::string_view op,
                        std::uint64_t ts_ms,
                        std::initializer_list<std::pair<std::string_view, std::string>> fields = {}) {
    Trace::instance().event(domain, op, ts_ms, fields);
}

}  // namespace oa::util
