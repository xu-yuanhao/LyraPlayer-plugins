#pragma once

#include <string>
#include <unordered_map>
#include <utility>

struct TomlEntry {
    enum class Kind { String, Number, Bool };
    Kind kind = Kind::String;
    std::string text;
    double number = 0;
    bool flag = false;
};

class Toml {
public:
    const std::unordered_map<std::string,TomlEntry>& entries()const{return entries_;}
    void set(std::string key,TomlEntry value){entries_[std::move(key)]=std::move(value);}
    bool parse(const std::string& text, std::string* error);
    bool overlay(const Toml& over, std::string* error, std::string* unknown = nullptr);
    const TomlEntry* find(const std::string& key) const;
    bool require_string(const char* key, std::string* out, std::string* error) const;
    bool require_number(const char* key, double* out, std::string* error) const;
    bool require_bool(const char* key, bool* out, std::string* error) const;

private:
    std::unordered_map<std::string, TomlEntry> entries_;
};

std::string read_text_file(const std::string& path);
