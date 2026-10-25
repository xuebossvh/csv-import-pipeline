#pragma once

#include "types.h"

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

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
    // Strict conversions: invalid/empty/partial input throws invalid_argument;
    // numeric overflow/underflow or an invalid column index throws out_of_range.
    // These helpers are also used by FieldValidator, so both APIs agree.
    static int parseInt(const std::string& value) {
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || end != value.c_str() + value.size()) {
            throw std::invalid_argument("Expected a complete integer");
        }
        if (errno == ERANGE || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            throw std::out_of_range("Integer is outside the int range");
        }
        return static_cast<int>(parsed);
    }

    static double parseDouble(const std::string& value) {
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || end != value.c_str() + value.size()) {
            throw std::invalid_argument("Expected a complete number");
        }
        if (errno == ERANGE) {
            throw std::out_of_range("Number is outside the double range");
        }
        if (!std::isfinite(parsed)) {
            throw std::invalid_argument("Expected a finite number");
        }
        return parsed;
    }

    int toInt(size_t index) const { return parseInt(fields_.at(index)); }

    double toDouble(size_t index) const { return parseDouble(fields_.at(index)); }

    std::optional<int> tryToInt(size_t index) const {
        try {
            return toInt(index);
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        } catch (const std::out_of_range&) {
            return std::nullopt;
        }
    }

    std::optional<double> tryToDouble(size_t index) const {
        try {
            return toDouble(index);
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        } catch (const std::out_of_range&) {
            return std::nullopt;
        }
    }

    const std::string& toString(size_t index) const {
        static const std::string empty;
        if (index >= fields_.size()) return empty;
        return fields_[index];
    }

    bool toBool(size_t index) const {
        const auto& s = fields_.at(index);
        if (s == "true" || s == "TRUE" || s == "1" || s == "yes") return true;
        if (s == "false" || s == "FALSE" || s == "0" || s == "no") return false;
        throw std::invalid_argument("Expected a boolean");
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
