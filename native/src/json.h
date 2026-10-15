// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal JSON writer. The native side returns JSON strings and the TypeScript
// side parses them: this keeps the N-API surface tiny (no object building, no
// dependency on any JSON library) and makes the bridge easy to inspect.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace saf {

inline std::string jsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

class JsonObject {
public:
    explicit JsonObject(std::string& out) : out_(out) { out_ += '{'; }

    template <typename T>
    JsonObject& number(const std::string& key, T value) {
        separator();
        out_ += "\"" + key + "\":" + std::to_string(value);
        return *this;
    }

    JsonObject& string(const std::string& key, const std::string& value) {
        separator();
        out_ += "\"" + key + "\":\"" + jsonEscape(value) + "\"";
        return *this;
    }

    JsonObject& boolean(const std::string& key, bool value) {
        separator();
        out_ += "\"" + key + "\":" + (value ? "true" : "false");
        return *this;
    }

    JsonObject& raw(const std::string& key, const std::string& json) {
        separator();
        out_ += "\"" + key + "\":" + json;
        return *this;
    }

    ~JsonObject() { out_ += '}'; }

private:
    void separator() {
        if (!first_) out_ += ',';
        first_ = false;
    }

    std::string& out_;
    bool first_ = true;
};

inline std::string jsonStringArray(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ',';
        out += "\"" + jsonEscape(values[i]) + "\"";
    }
    out += ']';
    return out;
}

inline std::string jsonNumberArray(const std::vector<uint32_t>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ',';
        out += std::to_string(values[i]);
    }
    out += ']';
    return out;
}

} // namespace saf
