#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace csvp {

// ---------- error codes ----------
enum class ErrorCode : int {
    Ok = 0,
    ParseError,            // 解析阶段错误
    ColumnCountMismatch,   // 列数不匹配
    TypeConversionError,   // 类型转换失败
    RequiredFieldMissing,  // 必填字段为空
    RangeError,            // 数值超出范围
    RegexMismatch,         // 正则不匹配
    EncodingError,         // 编码错误
    FileNotOpen,           // 文件打开失败
    InternalError,         // 内部错误
    IOError,              // 文件读取失败（不能作为坏行跳过）
};

// ---------- validation rule ----------
enum class FieldType {
    String,
    Int,
    Double,
    Bool,
};

struct FieldRule {
    std::string name;          // 字段名称
    FieldType type = FieldType::String;
    bool required = false;
    double min_val = 0;        // 数值范围下限
    double max_val = 0;        // 数值范围上限
    bool has_range = false;
    std::string regex;         // 正则校验（空=不校验）
    std::string default_val;   // 默认值（空=不设默认）
};

// ---------- per-cell error ----------
struct CellError {
    size_t column_index = 0;
    std::string column_name;
    std::string raw_value;
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    bool details_truncated = false;
};

// ---------- per-row error ----------
struct RowError {
    uint64_t line_number = 0;
    std::string raw_line;      // 原始行（不含行尾）；受预算截断时看details_truncated
    std::vector<CellError> cell_errors;
    bool details_truncated = false;
};

// ---------- pipeline stats ----------
struct Stats {
    uint64_t total_lines = 0;
    uint64_t bytes_read = 0;
    uint64_t rows_imported = 0;
    uint64_t rows_skipped = 0;
    double elapsed_ms = 0.0;
    uint64_t error_count = 0;
    bool errors_truncated = false;
};

} // namespace csvp
