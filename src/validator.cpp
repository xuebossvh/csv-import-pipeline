#include "csvp/validator.h"

#include <cstdlib>
#include <sstream>

namespace csvp {

void FieldValidator::addRule(const FieldRule& rule) {
    rules_.push_back(rule);
}

void FieldValidator::addRules(const std::vector<FieldRule>& rules) {
    rules_ = rules;
}

std::vector<CellError> FieldValidator::validate(const Row& row) const {
    std::vector<CellError> errors;

    size_t col_count = row.size();
    if (col_count > rules_.size()) {
        col_count = rules_.size();
    }

    for (size_t i = 0; i < col_count; ++i) {
        const auto& raw = row[i];
        auto err = validateField(i, raw, rules_[i]);
        if (err.code != ErrorCode::Ok) {
            errors.push_back(std::move(err));
        }
    }

    return errors;
}

CellError FieldValidator::validateField(size_t column_index,
                                         const std::string& raw_value,
                                         const FieldRule& rule) const {
    CellError err;
    err.column_index = column_index;
    err.column_name = rule.name;
    err.raw_value = raw_value;

    // 检查必填
    if (rule.required && raw_value.empty()) {
        err.code = ErrorCode::RequiredFieldMissing;
        err.message = "Required field is empty";
        return err;
    }

    // 空值 + 非必填：跳过后续校验
    if (raw_value.empty()) {
        err.code = ErrorCode::Ok;
        return err;
    }

    // 类型校验
    std::string type_err;
    if (!validateType(raw_value, rule.type, type_err)) {
        err.code = ErrorCode::TypeConversionError;
        err.message = type_err;
        return err;
    }

    // 范围校验
    std::string range_err;
    if (rule.has_range && !validateRange(raw_value, rule, range_err)) {
        err.code = ErrorCode::RangeError;
        err.message = range_err;
        return err;
    }

    // 正则校验
    std::string regex_err;
    if (!rule.regex.empty() && !validateRegex(raw_value, rule.regex, regex_err)) {
        err.code = ErrorCode::RegexMismatch;
        err.message = regex_err;
        return err;
    }

    err.code = ErrorCode::Ok;
    return err;
}

bool FieldValidator::validateType(const std::string& value, FieldType type,
                                  std::string& err_msg) const {
    switch (type) {
    case FieldType::Int: {
        // 使用 strtol 而非 stoi：strtol 可以通过 endptr 判断
        // 是否完整转换，但当前实现回到了简单的错误处理。
        char* end = nullptr;
        long val = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0') {
            err_msg = "Expected integer, got: " + value;
            return false;
        }
        (void)val;
        return true;
    }
    case FieldType::Double: {
        char* end = nullptr;
        double val = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0') {
            err_msg = "Expected number, got: " + value;
            return false;
        }
        (void)val;
        return true;
    }
    case FieldType::Bool: {
        if (value != "true" && value != "false" &&
            value != "TRUE" && value != "FALSE" &&
            value != "1" && value != "0" &&
            value != "yes" && value != "no") {
            err_msg = "Expected boolean, got: " + value;
            return false;
        }
        return true;
    }
    case FieldType::String:
    default:
        return true;
    }
}

bool FieldValidator::validateRange(const std::string& value, const FieldRule& rule,
                                   std::string& err_msg) const {
    if (rule.type == FieldType::Int) {
        char* end = nullptr;
        long val = std::strtol(value.c_str(), &end, 10);
        if (val < static_cast<long>(rule.min_val) ||
            val > static_cast<long>(rule.max_val)) {
            std::ostringstream oss;
            oss << "Value " << val << " out of range ["
                << rule.min_val << ", " << rule.max_val << "]";
            err_msg = oss.str();
            return false;
        }
    } else if (rule.type == FieldType::Double) {
        char* end = nullptr;
        double val = std::strtod(value.c_str(), &end);
        if (val < rule.min_val || val > rule.max_val) {
            std::ostringstream oss;
            oss << "Value " << val << " out of range ["
                << rule.min_val << ", " << rule.max_val << "]";
            err_msg = oss.str();
            return false;
        }
    }
    return true;
}

bool FieldValidator::validateRegex(const std::string& value, const std::string& pattern,
                                   std::string& err_msg) const {
    try {
        std::regex re(pattern);
        if (!std::regex_match(value, re)) {
            err_msg = "Value does not match pattern: " + pattern;
            return false;
        }
    } catch (const std::regex_error& e) {
        err_msg = std::string("Invalid regex pattern: ") + e.what();
        return false;
    }
    return true;
}

} // namespace csvp