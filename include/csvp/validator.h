#pragma once

#include "types.h"
#include "row.h"

#include <string>
#include <vector>
#include <regex>

namespace csvp {

// FieldValidator: 对一行 CSV 数据的每个字段按规则校验。
// 校验规则由外部通过 addRule() 或构造函数传入。
class FieldValidator {
public:
    struct ValidationResult {
        bool valid = true;
        std::vector<CellError> errors;
        size_t storage_bytes = 0;
        bool details_truncated = false;
    };

    FieldValidator() = default;

    // 添加一条校验规则（按列索引顺序）
    void addRule(const FieldRule& rule);
    void addRules(const std::vector<FieldRule>& rules);

    // 校验一整行。返回错误列表（空 = 全部通过）。
    std::vector<CellError> validate(const Row& row) const;

    // Zero budget checks validity without copying rejected field contents.
    ValidationResult validateBounded(const Row& row, size_t max_detail_bytes) const;

    // 校验单个字段
    CellError validateField(size_t column_index, const std::string& raw_value,
                            const FieldRule& rule) const;

    const std::vector<FieldRule>& rules() const { return rules_; }
    size_t columnCount() const { return rules_.size(); }

private:
    ErrorCode checkField(const std::string& value, const FieldRule& rule,
                        std::string& message) const;
    bool validateType(const std::string& value, FieldType type, std::string& err_msg) const;
    bool validateRange(const std::string& value, const FieldRule& rule, std::string& err_msg) const;
    bool validateRegex(const std::string& value, const std::string& pattern, std::string& err_msg) const;

    std::vector<FieldRule> rules_;
};

} // namespace csvp
