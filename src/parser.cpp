#include "csvp/parser.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace csvp {

// ============================================================
// internal helpers
// ============================================================

static size_t getFileSize(FILE* f) {
#ifdef _WIN32
    auto fd = _fileno(f);
    return static_cast<size_t>(_filelengthi64(fd));
#else
    struct stat st;
    fstat(fileno(f), &st);
    return static_cast<size_t>(st.st_size);
#endif
}

// ============================================================
// CsvParser
// ============================================================

CsvParser::CsvParser(const Config& cfg) : cfg_(cfg) {}

CsvParser::~CsvParser() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    free(line_buffer_);
    line_buffer_ = nullptr;
}

// ---------- parse ----------
bool CsvParser::parse(const char* filepath) {
    // 关闭之前打开的文件
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }

    // 打开文件
#ifdef _WIN32
    fopen_s(&file_, filepath, "rb");
#else
    file_ = fopen(filepath, "rb");
#endif
    if (!file_) {
        last_error_ = std::string("Failed to open file: ") + filepath;
        return false;
    }

    file_size_ = getFileSize(file_);

    // 分配行缓冲区（最大 16 MB）
    buffer_capacity_ = (1 << 24);  // 16 MB
    line_buffer_ = static_cast<char*>(malloc(buffer_capacity_));
    if (!line_buffer_) {
        last_error_ = "Failed to allocate line buffer";
        return false;
    }

    // ---- 状态重置 ----
    header_.clear();
    header_parsed_ = false;
    buffer_offset_ = 0;

    // 解析表头
    if (cfg_.has_header) {
        Row header_row;
        if (!readRow(header_row)) {
            last_error_ = "Failed to read header row";
            return false;
        }
        for (size_t i = 0; i < header_row.size(); ++i) {
            header_.push_back(header_row[i]);
        }
        header_parsed_ = true;
    }

    return true;
}

// ---------- readRow ----------
bool CsvParser::readRow(Row& row) {
    if (!file_) {
        last_error_ = "No file opened";
        return false;
    }

    row.clear();

    // 读取下一行到 line_buffer_
    if (!readLineFromFile()) {
        return false;
    }

    size_t line_len = strlen(line_buffer_);

    // 去掉末尾的 \r（Windows 换行）
    if (line_len > 0 && line_buffer_[line_len - 1] == '\r') {
        line_buffer_[line_len - 1] = '\0';
        --line_len;
    }

    // 跳过空行
    if (line_len == 0) {
        ++line_number_;
        return readRow(row);  // 递归跳过空行
    }

    // 分割字段
    if (!splitLineToFields(line_buffer_, line_len, row)) {
        ++line_number_;
        return false;
    }

    ++line_number_;
    return true;
}

// ---------- readLineFromFile ----------
bool CsvParser::readLineFromFile() {
    if (buffer_offset_ >= file_size_) {
        // 文件读取完毕。
        return false;
    }

    // 从当前偏移开始，向后搜索换行符
    size_t start = buffer_offset_;
    size_t search_len = (file_size_ - start < buffer_capacity_ - 1)
                            ? file_size_ - start
                            : buffer_capacity_ - 1;

    // 一次性读取一块到 line_buffer_
    fseek(file_, static_cast<long>(start), SEEK_SET);
    size_t bytes_to_read = std::min(search_len, buffer_capacity_ - 1);

    size_t nread = fread(line_buffer_, 1, bytes_to_read, file_);
    if (nread == 0) {
        return false;
    }
    line_buffer_[nread] = '\0';

    // 在读取的块中查找第一个 \n
    char* nl = strchr(line_buffer_, '\n');
    if (nl) {
        *nl = '\0';
        size_t line_bytes = static_cast<size_t>(nl - line_buffer_);
        buffer_offset_ = start + line_bytes + 1;  // +1 跳过 \n
        bytes_read_ += line_bytes + 1;
    } else {
        buffer_offset_ = start + nread;
        bytes_read_ += nread;
        return false;
    }

    return true;
}

// ---------- splitLineToFields ----------
bool CsvParser::splitLineToFields(const char* line, size_t length, Row& row) {
    row.clear();

    size_t pos = 0;
    std::string field;
    field.reserve(256);

    while (pos < length) {
        char c = line[pos];

        // 跳过前导空格（trim_spaces 模式）
        if (cfg_.trim_spaces && (c == ' ' || c == '\t') && field.empty()) {
            ++pos;
            continue;
        }

        // 引号字段处理
        if (c == '"') {
            if (!handleQuotedField(line, pos, length, field)) {
                return false;
            }
            // 跳过引号字段后的空格
            while (pos < length && cfg_.trim_spaces && (line[pos] == ' ' || line[pos] == '\t')) {
                ++pos;
            }
            // 期望分隔符或行尾
            if (pos < length && line[pos] != cfg_.delimiter) {
                // 引号字段后面应该是分隔符或行尾
                last_error_ = "Unexpected character after quoted field";
                return false;
            }
            if (pos < length) ++pos;  // 跳过分隔符
            row.append(field);
            field.clear();
            continue;
        }

        // 普通字段：读到分隔符或行尾
        if (c == cfg_.delimiter) {
            row.append(field);
            field.clear();
            ++pos;
            continue;
        }

        // 常规字符
        field += c;
        ++pos;
    }

    // 最后一个字段
    if (!field.empty() || pos > 0) {
        // 尾部空格修剪
        if (cfg_.trim_spaces) {
            while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) {
                field.pop_back();
            }
        }
        row.append(field);
    }

    return true;
}

// ---------- handleQuotedField ----------
bool CsvParser::handleQuotedField(const char* line, size_t& pos, size_t length, std::string& field) {
    // 跳过开始的引号
    ++pos;  // 跳过第一个 '"'

    while (pos < length) {
        char c = line[pos];

        if (c == '"') {
            ++pos;  // 闭合引号，结束字段
            return true;
        } else {
            field += c;
            ++pos;
        }
    }

    // 引号未闭合
    last_error_ = "Unclosed quote in CSV field";
    return false;
}

// ---------- isTrailingByte ----------
bool CsvParser::isTrailingByte(unsigned char c) const {
    // UTF-8 后续字节范围：0x80 ~ 0xBF
    return (c >= 0x80 && c <= 0xBF);
}

// ---------- accessors ----------
int CsvParser::lineNumber() const {
    return line_number_;
}

size_t CsvParser::bytesRead() const {
    return bytes_read_;
}

} // namespace csvp