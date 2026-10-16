#include "config/toml.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

std::string trim(const std::string& in) {
    size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) begin += 1;
    size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) end -= 1;
    return in.substr(begin, end - begin);
}

bool parse_string(const std::string& raw, std::string* out, std::string* error, int line) {
    if (raw.size() < 2 || raw[0] != '"') {
        *error = "line " + std::to_string(line) + ": expected string";
        return false;
    }
    std::string value;
    for (size_t i = 1; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '"') {
            if (trim(raw.substr(i + 1)).empty()) {
                *out = value;
                return true;
            }
            *error = "line " + std::to_string(line) + ": trailing junk";
            return false;
        }
        if (c == '\\') {
            if (i + 1 >= raw.size()) {
                *error = "line " + std::to_string(line) + ": broken escape";
                return false;
            }
            char n = raw[++i];
            if (n == 'n') value.push_back('\n');
            else if (n == 't') value.push_back('\t');
            else if (n == '"' || n == '\\') value.push_back(n);
            else {
                *error = "line " + std::to_string(line) + ": bad escape";
                return false;
            }
            continue;
        }
        value.push_back(c);
    }
    *error = "line " + std::to_string(line) + ": unterminated string";
    return false;
}

bool parse_scalar(const std::string& raw, TomlEntry* entry, std::string* error, int line) {
    if (!raw.empty() && raw[0] == '"') {
        entry->kind = TomlEntry::Kind::String;
        return parse_string(raw, &entry->text, error, line);
    }
    if (raw == "true" || raw == "false") {
        entry->kind = TomlEntry::Kind::Bool;
        entry->flag = raw == "true";
        entry->text = raw;
        return true;
    }
    char* end = nullptr;
    entry->number = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str() || trim(end).size() != 0) {
        *error = "line " + std::to_string(line) + ": expected value";
        return false;
    }
    entry->kind = TomlEntry::Kind::Number;
    entry->text = raw;
    return true;
}

}  // namespace

bool Toml::parse(const std::string& text, std::string* error) {
    entries_.clear();
    std::string section;
    int line = 1;
    size_t i = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) i = 3;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(i, end - i);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        i = end < text.size() ? end + 1 : end;

        std::string row = trim(raw);
        if (!row.empty() && row[0] != '#') {
            if (row[0] == '[') {
                if (row.back() != ']') {
                    *error = "line " + std::to_string(line) + ": unclosed table";
                    return false;
                }
                section = trim(row.substr(1, row.size() - 2));
                if (section.empty()) {
                    *error = "line " + std::to_string(line) + ": empty table";
                    return false;
                }
            } else {
                size_t eq = row.find('=');
                if (eq == std::string::npos) {
                    *error = "line " + std::to_string(line) + ": expected '='";
                    return false;
                }
                std::string key = trim(row.substr(0, eq));
                std::string value = trim(row.substr(eq + 1));
                if (key.empty()) {
                    *error = "line " + std::to_string(line) + ": empty key";
                    return false;
                }
                std::string full = section.empty() ? key : section + "." + key;
                TomlEntry entry;
                if (!parse_scalar(value, &entry, error, line)) return false;
                entries_[full] = entry;
            }
        }
        line += 1;
    }
    return true;
}

bool Toml::overlay(const Toml& over, std::string* error, std::string* unknown) {
    for (const auto& item : over.entries_) {
        auto found = entries_.find(item.first);
        if (found == entries_.end()) continue;
        if (found->second.kind != item.second.kind) {
            if (error) *error = "bad type " + item.first;
            return false;
        }
    }
    if (unknown != nullptr) unknown->clear();
    auto append = [&](const std::string& key, const TomlEntry& entry) {
        if (unknown == nullptr) return;
        *unknown += key;
        *unknown += "=";
        if (entry.kind == TomlEntry::Kind::String) {
            *unknown += "\"";
            *unknown += entry.text;
            *unknown += "\"";
        } else if (entry.kind == TomlEntry::Kind::Bool) {
            *unknown += entry.flag ? "true" : "false";
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", entry.number);
            *unknown += buf;
        }
        *unknown += "\n";
    };
    for (const auto& item : over.entries_) {
        auto found = entries_.find(item.first);
        if (found == entries_.end()) {
            append(item.first, item.second);
            continue;
        }
        found->second = item.second;
    }
    return true;
}

std::string read_text_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return {};
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return {};
    }
    long size = std::ftell(file);
    if (size <= 0 || size > 1024 * 1024) {
        std::fclose(file);
        return {};
    }
    std::rewind(file);
    std::string text((size_t)size, '\0');
    size_t got = std::fread(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (got != text.size()) return {};
    return text;
}

const TomlEntry* Toml::find(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    return &it->second;
}

bool Toml::require_string(const char* key, std::string* out, std::string* error) const {
    const TomlEntry* entry = find(key);
    if (entry == nullptr || entry->kind != TomlEntry::Kind::String) {
        *error = std::string("missing string ") + key;
        return false;
    }
    *out = entry->text;
    return true;
}

bool Toml::require_number(const char* key, double* out, std::string* error) const {
    const TomlEntry* entry = find(key);
    if (entry == nullptr || entry->kind != TomlEntry::Kind::Number) {
        *error = std::string("missing number ") + key;
        return false;
    }
    *out = entry->number;
    return true;
}

bool Toml::require_bool(const char* key, bool* out, std::string* error) const {
    const TomlEntry* entry = find(key);
    if (entry == nullptr || entry->kind != TomlEntry::Kind::Bool) {
        *error = std::string("missing bool ") + key;
        return false;
    }
    *out = entry->flag;
    return true;
}
