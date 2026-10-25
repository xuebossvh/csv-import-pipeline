#pragma once

#include "types.h"
#include "row.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace csvp {

// 顺序分块读取 CSV；字段保留所选编码的原始字节，不进行转码。
// 支持 UTF-8 和 GBK、双引号转义，不支持引号字段跨越物理行。
class CsvParser {
public:
    struct Config {
        char delimiter = ',';
        bool has_header = true;
        bool trim_spaces = true;
        std::string encoding = "UTF-8";
        // 物理行内容的字节上限，不含 LF/CRLF；UTF-8 BOM 计入该上限。
        std::size_t max_line_bytes = (std::size_t{1} << 24) - 1;
    };

    enum class ReadStatus { Row, End, Error };

    CsvParser() = default;
    explicit CsvParser(const Config& cfg);
    ~CsvParser() = default;

    CsvParser(const CsvParser&) = delete;
    CsvParser& operator=(const CsvParser&) = delete;
    CsvParser(CsvParser&&) = delete;
    CsvParser& operator=(CsvParser&&) = delete;

    // filepath 始终按 UTF-8 解释；每次调用关闭旧文件并重置所有状态。
    // has_header=true 时读取第一条非空物理行作为表头。
    bool parse(const char* filepath);
    const std::vector<std::string>& header() const { return header_; }

    // 行级错误已消费该物理行，可以继续读取下一行；致命错误后返回 End。
    // 空物理行被跳过，但计入 lineNumber() 和 bytesRead()。
    ReadStatus readNext(Row& row);
    bool readRow(Row& row); // 兼容接口：仅 ReadStatus::Row 返回 true。

    std::uint64_t lineNumber() const { return line_number_; }
    // 已消费的文件字节，包括表头和换行符，不包括缓冲预读的未消费数据。
    std::uint64_t bytesRead() const { return bytes_read_; }
    const std::string& lastError() const { return last_error_; }
    ErrorCode lastErrorCode() const { return last_error_code_; }
    // 保留引号、分隔符和空白，去除 LF/CRLF；超长行仅保留上限以内的前缀。
    const std::string& rawLine() const { return raw_line_; }
    bool rawLineTruncated() const { return raw_line_truncated_; }

private:
    enum class Encoding { Utf8, Gbk };
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept;
    };

    ReadStatus readLineFromFile();
    bool splitLineToFields(std::size_t begin, Row& row);
    std::size_t characterLength(std::size_t pos) const;
    void setError(ErrorCode code, const std::string& message);
    bool countLine();

    Config cfg_;
    Encoding encoding_ = Encoding::Utf8;
    std::unique_ptr<std::FILE, FileCloser> file_;
    std::array<char, 64 * 1024> input_buffer_{};
    std::size_t buffer_pos_ = 0;
    std::size_t buffer_end_ = 0;
    bool finished_ = false;

    std::vector<std::string> header_;
    std::string raw_line_;
    bool raw_line_truncated_ = false;
    std::string last_error_;
    ErrorCode last_error_code_ = ErrorCode::Ok;
    std::uint64_t line_number_ = 0;
    std::uint64_t bytes_read_ = 0;
};

} // namespace csvp
