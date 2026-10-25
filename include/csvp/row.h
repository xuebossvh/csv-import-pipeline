#pragma once

#include "types.h"

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdlib>

namespace csvp {

// Row: 一行 CSV 数据的轻量容器。
// 存储解析后的字段（全部为字符串），提供类型转换访问器。
class Row {
public:
    Row() = default;

    explicit Row(std::vector<std::string> fields)
        : fields_(std::move(fields)) {}

    // ---------- raw access ----------
    size_t size() const { return fields_.size(); }
    bool empty() const { return fields_.empty(); }

    const std::string& operator[](size_t index) const {
        return fields_[index];
    }

    std::string& operator[](size_t index) {
        return fields_[index];
    }

    const std::string& at(size_t index) const {
        return fields_.at(index);
    }

    // ---------- typed accessors ----------
    int toInt(size_t index) const {
        if (index >= fields_.size()) return 0;
        const auto& s = fields_[index];
        if (s.empty()) return 0;
        try {
            return std::stoi(s);
        } catch (...) {
            return 0;
        }
    }

    double toDouble(size_t index) const {
        if (index >= fields_.size()) return 0.0;
        const auto& s = fields_[index];
        if (s.empty()) return 0.0;
        return std::atof(s.c_str());
    }

    const std::string& toString(size_t index) const {
        static const std::string empty;
        if (index >= fields_.size()) return empty;
        return fields_[index];
    }

    bool toBool(size_t index) const {
        if (index >= fields_.size()) return false;
        const auto& s = fields_[index];
        return s == "true" || s == "TRUE" || s == "1" || s == "yes";
    }

    // ---------- internal ----------
    void append(const std::string& field) { fields_.push_back(field); }
    void clear() { fields_.clear(); is_valid_ = true; }
    bool isValid() const { return is_valid_; }
    void setValid(bool v) { is_valid_ = v; }

private:
    std::vector<std::string> fields_;
    bool is_valid_ = true;
};

} // namespace csvp