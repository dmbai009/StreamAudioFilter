// StreamAudioFilter — remove only one plugin's entry from shared JSON settings.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cctype>

namespace saf::setup {
// A structural reader, not a regex: nested strings/objects and escaped keys
// must never cause another plugin's settings to be removed. Preserve all bytes
// outside the selected member. Reject malformed JSON before changing anything.
class SettingsJson {
    struct Member { size_t begin, end; std::string key; };
    const std::string& text_;
    size_t pos_ = 0;
    std::vector<std::pair<size_t, size_t>> removals_;
    void ws() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) pos_++; }
    void expect(char c) { ws(); if (pos_ >= text_.size() || text_[pos_++] != c) throw std::runtime_error("invalid settings JSON"); }
    std::string string() {
        expect('"');
        std::string result;
        bool closed = false;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') { closed = true; break; }
            if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("invalid JSON string");
            if (c == '\\') {
                if (pos_ >= text_.size()) throw std::runtime_error("invalid escape");
                c = text_[pos_++];
                if (c == 'u') {
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        if (pos_ >= text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[pos_]))) throw std::runtime_error("invalid unicode escape");
                        const char h = text_[pos_++];
                        code = code * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                    }
                    result += code < 128 ? static_cast<char>(code) : '?';
                    continue;
                }
                if (std::string("\"\\/bfnrt").find(c) == std::string::npos) throw std::runtime_error("invalid escape");
            }
            result += c;
        }
        if (!closed) throw std::runtime_error("unterminated string");
        return result;
    }
    void value(int depth, int path) {
        if (depth > 128) throw std::runtime_error("settings JSON too deep");
        ws();
        if (pos_ >= text_.size()) throw std::runtime_error("missing value");
        if (text_[pos_] == '{') {
            pos_++; ws();
            std::vector<Member> members;
            if (pos_ < text_.size() && text_[pos_] != '}') {
                while (true) {
                    ws(); const size_t begin = pos_;
                    const auto key = string(); expect(':');
                    value(depth + 1, path == 0 && key == "plugins" ? 1 : -1);
                    members.push_back({begin, pos_, key}); ws();
                    if (pos_ >= text_.size() || text_[pos_] != ',') break;
                    pos_++;
                }
            }
            expect('}');
            size_t matches = 0;
            for (const auto& member : members) if (path == 1 && member.key == "StreamAudioFilter") matches++;
            if (matches > 1) throw std::runtime_error("duplicate plugin settings key");
            for (size_t i = 0; path == 1 && i < members.size(); i++) {
                if (members[i].key != "StreamAudioFilter") continue;
                const size_t begin = i ? members[i - 1].end : members[i].begin;
                const size_t end = !i && members.size() > 1 ? members[1].begin : members[i].end;
                removals_.emplace_back(begin, end);
            }
        } else if (text_[pos_] == '[') {
            pos_++; ws();
            if (pos_ < text_.size() && text_[pos_] != ']') {
                while (true) { value(depth + 1, -1); ws(); if (pos_ >= text_.size() || text_[pos_] != ',') break; pos_++; }
            }
            expect(']');
        } else if (text_[pos_] == '"') { string(); }
        else {
            const size_t begin = pos_;
            while (pos_ < text_.size() && std::string(",]} \t\r\n").find(text_[pos_]) == std::string::npos) pos_++;
            const auto token = text_.substr(begin, pos_ - begin);
            if (token == "true" || token == "false" || token == "null") return;
            // Validate the JSON number grammar without locale-dependent parsing.
            size_t n = 0;
            if (n < token.size() && token[n] == '-') n++;
            if (n < token.size() && token[n] == '0') n++;
            else { const auto start = n; while (n < token.size() && std::isdigit(static_cast<unsigned char>(token[n]))) n++; if (n == start) throw std::runtime_error("invalid value"); }
            if (n < token.size() && token[n] == '.') { const auto start = ++n; while (n < token.size() && std::isdigit(static_cast<unsigned char>(token[n]))) n++; if (n == start) throw std::runtime_error("invalid number"); }
            if (n < token.size() && (token[n] == 'e' || token[n] == 'E')) { n++; if (n < token.size() && (token[n] == '+' || token[n] == '-')) n++; const auto start = n; while (n < token.size() && std::isdigit(static_cast<unsigned char>(token[n]))) n++; if (n == start) throw std::runtime_error("invalid exponent"); }
            if (n != token.size()) throw std::runtime_error("invalid value");
        }
    }
public:
    explicit SettingsJson(const std::string& text) : text_(text) {}
    std::string withoutPlugin() {
        value(0, 0); ws();
        if (pos_ != text_.size()) throw std::runtime_error("trailing settings data");
        std::string result = text_;
        for (auto it = removals_.rbegin(); it != removals_.rend(); ++it) result.erase(it->first, it->second - it->first);
        return result;
    }
};
}
