#include "csvp/validator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace csvp {
namespace {

void copyWithinBudget(std::string& output, const std::string& input,
                      size_t& budget, bool& truncated) {
    const size_t length = std::min(input.size(), budget);
    output.assign(input.data(), length);
    budget -= length;
    truncated = truncated || length != input.size();
}

} // namespace

void FieldValidator::addRule(const FieldRule& rule) {
    rules_.push_back(rule);
}

void FieldValidator::addRules(const std::vector<FieldRule>& rules) {
    rules_ = rules;
}

std::vector<CellError> FieldValidator::validate(const Row& row) const {
    return validateBounded(row, std::numeric_limits<size_t>::max()).errors;
}

FieldValidator::ValidationResult FieldValidator::validateBounded(
    const Row& row, size_t max_detail_bytes) const {
    ValidationResult result;
    size_t remaining = max_detail_bytes;
    static const std::string empty;

    for (size_t i = 0; i < rules_.size(); ++i) {
        const bool absent = i >= row.size();
        if (absent && !rules_[i].required) continue;
        const std::string& value = absent ? empty : row[i];
        std::string message;
        const ErrorCode code = absent ? ErrorCode::RequiredFieldMissing
                                     : checkField(value, rules_[i], message);
        if (code == ErrorCode::Ok) continue;
        if (absent) message = "Required column is missing";
        result.valid = false;

        if (remaining < sizeof(CellError)) {
            result.details_truncated = true;
            break; // Row validity is known; no more details can be retained.
        }
        CellError error;
        error.column_index = i;
        error.code = code;
        remaining -= sizeof(CellError);
        copyWithinBudget(error.column_name, rules_[i].name, remaining,
                         error.details_truncated);
        copyWithinBudget(error.message, message, remaining, error.details_truncated);
        copyWithinBudget(error.raw_value, value, remaining, error.details_truncated);
        result.details_truncated = result.details_truncated || error.details_truncated;
        result.errors.push_back(std::move(error));
    }
    result.storage_bytes = max_detail_bytes - remaining;
    return result;
}

CellError FieldValidator::validateField(size_t column_index,
                                       const std::string& raw_value,
                                       const FieldRule& rule) const {
    CellError error;
    error.column_index = column_index;
    error.column_name = rule.name;
    error.raw_value = raw_value;
    error.code = checkField(raw_value, rule, error.message);
    return error;
}

ErrorCode FieldValidator::checkField(const std::string& value, const FieldRule& rule,
                                    std::string& message) const {
    if (value.empty()) {
        if (rule.required) {
            message = "Required field is empty";
            return ErrorCode::RequiredFieldMissing;
        }
        return ErrorCode::Ok;
    }
    if (!validateType(value, rule.type, message)) return ErrorCode::TypeConversionError;
    if (rule.has_range && !validateRange(value, rule, message)) return ErrorCode::RangeError;
    if (!rule.regex.empty() && !validateRegex(value, rule.regex, message)) {
        return ErrorCode::RegexMismatch;
    }
    return ErrorCode::Ok;
}

bool FieldValidator::validateType(const std::string& value, FieldType type,
                                  std::string& message) const {
    try {
        switch (type) {
        case FieldType::Int:
            (void)Row::parseInt(value);
            return true;
        case FieldType::Double:
            (void)Row::parseDouble(value);
            return true;
        case FieldType::Bool:
            if (value == "true" || value == "false" || value == "TRUE" ||
                value == "FALSE" || value == "1" || value == "0" ||
                value == "yes" || value == "no") return true;
            message = "Expected a boolean";
            return false;
        case FieldType::String:
            return true;
        }
        message = "Unknown field type";
        return false;
    } catch (const std::invalid_argument& error) {
        message = error.what();
    } catch (const std::out_of_range& error) {
        message = error.what();
    }
    return false;
}

bool FieldValidator::validateRange(const std::string& value, const FieldRule& rule,
                                   std::string& message) const {
    if (rule.type != FieldType::Int && rule.type != FieldType::Double) return true;
    if (!std::isfinite(rule.min_val) || !std::isfinite(rule.max_val) ||
        rule.min_val > rule.max_val) {
        message = "Invalid numeric range rule";
        return false;
    }
    // Compare original bounds; narrowing fractional bounds to long loses data.
    const double parsed = rule.type == FieldType::Int
        ? static_cast<double>(Row::parseInt(value)) : Row::parseDouble(value);
    if (parsed < rule.min_val || parsed > rule.max_val) {
        message = "Value is outside the configured range";
        return false;
    }
    return true;
}

bool FieldValidator::validateRegex(const std::string& value, const std::string& pattern,
                                   std::string& message) const {
    try {
        const std::regex expression(pattern);
        if (!std::regex_match(value, expression)) {
            message = "Value does not match the configured pattern";
            return false;
        }
    } catch (const std::regex_error& error) {
        message = std::string("Invalid regex pattern: ") + error.what();
        return false;
    }
    return true;
}

} // namespace csvp
