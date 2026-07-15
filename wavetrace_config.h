#pragma once

// Lightweight, header-only reader for the WaveTrace project configuration.
// The build integration defines WAVETRACE_CONFIG_PATH as an absolute path to
// the source-tree WaveTracer/wavetrace_config.json. Standalone users can define
// the macro themselves; otherwise the current directory is used.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#if defined(WAVETRACE_CONFIG_PATH)
#define WAVETRACE_CONFIG_PATH_IS_AUTHORITATIVE 1
#else
#define WAVETRACE_CONFIG_PATH "wavetrace_config.json"
#define WAVETRACE_CONFIG_PATH_IS_AUTHORITATIVE 0
#endif

namespace wave {
namespace config {

struct RuntimeConfig {
    bool loaded = false;
    bool valid = true;
    bool path_is_authoritative = WAVETRACE_CONFIG_PATH_IS_AUTHORITATIVE != 0;
    std::string path = WAVETRACE_CONFIG_PATH;
    std::string error;

    bool wave_trace = true;
    std::string wave_trace_file_name = "wave.wvz4";
    std::uint64_t wave_trace_start = 0;
    std::uint64_t wave_trace_end = (std::numeric_limits<std::uint64_t>::max)();

    bool wave_trace_level_enabled = false;
    std::size_t wave_trace_level = 0;
    bool dirty_array_stats = false;
    bool dirty_array_marks = false;
    bool memory_usage = false;
};

namespace detail {

inline void skip_json_ws(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

inline int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool parse_json_hex4(const std::string& text, std::size_t& pos, std::uint32_t& value) {
    if (pos + 4 > text.size()) return false;
    value = 0;
    for (int i = 0; i != 4; ++i) {
        const int digit = json_hex_digit(text[pos++]);
        if (digit < 0) return false;
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    return true;
}

inline void append_utf8(std::string& value, std::uint32_t code_point) {
    if (code_point <= 0x7fu) {
        value.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffu) {
        value.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
        value.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else if (code_point <= 0xffffu) {
        value.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
        value.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        value.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else {
        value.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
        value.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
        value.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        value.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    }
}

inline bool parse_json_string(const std::string& text,
                              std::size_t& pos,
                              std::string& value) {
    skip_json_ws(text, pos);
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    value.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            value.push_back(c);
            continue;
        }
        if (pos >= text.size()) return false;
        const char escaped = text[pos++];
        switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            std::uint32_t code_point = 0;
            if (!parse_json_hex4(text, pos, code_point)) return false;
            if (code_point >= 0xd800u && code_point <= 0xdbffu) {
                if (pos + 2 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') return false;
                pos += 2;
                std::uint32_t low = 0;
                if (!parse_json_hex4(text, pos, low) || low < 0xdc00u || low > 0xdfffu) return false;
                code_point = 0x10000u + ((code_point - 0xd800u) << 10) + (low - 0xdc00u);
            } else if (code_point >= 0xdc00u && code_point <= 0xdfffu) {
                return false;
            }
            append_utf8(value, code_point);
            break;
        }
        default: return false;
        }
    }
    return false;
}

inline bool skip_json_value(const std::string& text, std::size_t& pos);

inline bool skip_json_number(const std::string& text, std::size_t& pos) {
    const std::size_t begin = pos;
    if (pos < text.size() && text[pos] == '-') ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == '0') {
        ++pos;
    } else {
        if (text[pos] < '1' || text[pos] > '9') return false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        const std::size_t digits = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (pos == digits) return false;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        const std::size_t digits = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (pos == digits) return false;
    }
    return pos != begin;
}

inline bool skip_json_value(const std::string& text, std::size_t& pos) {
    skip_json_ws(text, pos);
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
        std::string ignored;
        return parse_json_string(text, pos, ignored);
    }
    if (text[pos] == '{') {
        ++pos;
        skip_json_ws(text, pos);
        if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
        for (;;) {
            std::string key;
            if (!parse_json_string(text, pos, key)) return false;
            skip_json_ws(text, pos);
            if (pos >= text.size() || text[pos++] != ':') return false;
            if (!skip_json_value(text, pos)) return false;
            skip_json_ws(text, pos);
            if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
            if (pos >= text.size() || text[pos++] != ',') return false;
        }
    }
    if (text[pos] == '[') {
        ++pos;
        skip_json_ws(text, pos);
        if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
        for (;;) {
            if (!skip_json_value(text, pos)) return false;
            skip_json_ws(text, pos);
            if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
            if (pos >= text.size() || text[pos++] != ',') return false;
        }
    }
    if (text.compare(pos, 4, "true") == 0) { pos += 4; return true; }
    if (text.compare(pos, 5, "false") == 0) { pos += 5; return true; }
    if (text.compare(pos, 4, "null") == 0) { pos += 4; return true; }
    return skip_json_number(text, pos);
}

inline bool validate_json_object_document(const std::string& text) {
    std::size_t pos = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xefu &&
        static_cast<unsigned char>(text[1]) == 0xbbu &&
        static_cast<unsigned char>(text[2]) == 0xbfu) pos = 3;
    skip_json_ws(text, pos);
    if (pos >= text.size() || text[pos] != '{') return false;
    if (!skip_json_value(text, pos)) return false;
    skip_json_ws(text, pos);
    return pos == text.size();
}

inline bool find_top_level_value(const std::string& text,
                                 const char* wanted,
                                 std::size_t& value_pos) {
    std::size_t pos = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xefu &&
        static_cast<unsigned char>(text[1]) == 0xbbu &&
        static_cast<unsigned char>(text[2]) == 0xbfu) pos = 3;
    skip_json_ws(text, pos);
    if (pos >= text.size() || text[pos++] != '{') return false;
    int nested_depth = 0;
    while (pos < text.size()) {
        skip_json_ws(text, pos);
        if (pos >= text.size() || text[pos] == '}') return false;
        std::string key;
        if (!parse_json_string(text, pos, key)) return false;
        skip_json_ws(text, pos);
        if (pos >= text.size() || text[pos++] != ':') return false;
        skip_json_ws(text, pos);
        if (key == wanted) {
            value_pos = pos;
            return true;
        }

        bool in_string = false;
        bool escaped = false;
        nested_depth = 0;
        for (; pos < text.size(); ++pos) {
            const char c = text[pos];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == '{' || c == '[') { ++nested_depth; continue; }
            if (c == '}' || c == ']') {
                if (nested_depth > 0) { --nested_depth; continue; }
                return false;
            }
            if (c == ',' && nested_depth == 0) { ++pos; break; }
        }
    }
    return false;
}

inline bool read_bool(const std::string& text, const char* key, bool& value) {
    std::size_t pos = 0;
    if (!find_top_level_value(text, key, pos)) return true;
    if (text.compare(pos, 4, "true") == 0) { value = true; return true; }
    if (text.compare(pos, 5, "false") == 0) { value = false; return true; }
    std::string string_value;
    if (parse_json_string(text, pos, string_value)) {
        if (string_value == "1" || string_value == "true" || string_value == "TRUE") {
            value = true;
            return true;
        }
        if (string_value.empty() || string_value == "0" || string_value == "false" || string_value == "FALSE") {
            value = false;
            return true;
        }
    }
    return false;
}

inline bool read_string(const std::string& text, const char* key, std::string& value) {
    std::size_t pos = 0;
    if (!find_top_level_value(text, key, pos)) return true;
    return parse_json_string(text, pos, value);
}

inline bool parse_u64_text(const std::string& value, std::uint64_t& out) {
    if (value.empty()) return false;
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (result > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    out = result;
    return true;
}

inline bool read_optional_u64(const std::string& text,
                              const char* key,
                              std::uint64_t empty_value,
                              std::uint64_t& value) {
    std::size_t pos = 0;
    if (!find_top_level_value(text, key, pos)) return true;
    if (text.compare(pos, 4, "null") == 0) { value = empty_value; return true; }
    std::string token;
    if (pos < text.size() && text[pos] == '"') {
        if (!parse_json_string(text, pos, token)) return false;
        if (token.empty()) { value = empty_value; return true; }
    } else {
        const std::size_t begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        token.assign(text, begin, pos - begin);
    }
    return parse_u64_text(token, value);
}

inline RuntimeConfig load_runtime_config() {
    RuntimeConfig cfg;
    std::ifstream input(cfg.path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return cfg; // Missing config keeps backward-compatible defaults.

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        cfg.valid = false;
        cfg.error = "failed to read WaveTrace config";
        return cfg;
    }
    cfg.loaded = true;
    const std::string text = buffer.str();
    if (!validate_json_object_document(text)) {
        cfg.valid = false;
        cfg.error = "malformed WaveTrace JSON";
        return cfg;
    }

    std::uint64_t level = 0;
    const bool ok =
        read_bool(text, "WaveTrace", cfg.wave_trace) &&
        read_string(text, "WaveTraceFileName", cfg.wave_trace_file_name) &&
        read_optional_u64(text, "WaveTraceStart", 0, cfg.wave_trace_start) &&
        read_optional_u64(text, "WaveTraceEnd", (std::numeric_limits<std::uint64_t>::max)(), cfg.wave_trace_end) &&
        read_optional_u64(text, "WaveTraceLevel", 0, level) &&
        read_bool(text, "WaveTraceDirtyArrayStats", cfg.dirty_array_stats) &&
        read_bool(text, "WaveTraceDirtyArrayMarks", cfg.dirty_array_marks) &&
        read_bool(text, "WaveTraceMemoryUsage", cfg.memory_usage);
    if (!ok) {
        cfg.valid = false;
        cfg.error = "invalid WaveTrace config value";
        return cfg;
    }
    if (cfg.wave_trace_file_name.empty()) cfg.wave_trace_file_name = "wave.wvz4";
    if (level != 0) {
        cfg.wave_trace_level_enabled = true;
        cfg.wave_trace_level = level > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())
            ? (std::numeric_limits<std::size_t>::max)()
            : static_cast<std::size_t>(level);
    }
    if (cfg.wave_trace_end < cfg.wave_trace_start) {
        cfg.valid = false;
        cfg.error = "WaveTraceEnd is smaller than WaveTraceStart";
    }
    return cfg;
}

} // namespace detail

inline const RuntimeConfig& runtime_config() {
    static const RuntimeConfig cfg = detail::load_runtime_config();
    return cfg;
}

inline bool enabled() { return runtime_config().valid && runtime_config().wave_trace; }

inline bool cycle_in_range(std::uint64_t cycle) {
    const RuntimeConfig& cfg = runtime_config();
    return enabled() && cycle >= cfg.wave_trace_start && cycle <= cfg.wave_trace_end;
}

} // namespace config
} // namespace wave
