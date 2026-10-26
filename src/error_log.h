#pragma once

#include "csvp/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace csvp::detail {

// Internal fault-injection seam. Hooks obey C stdio semantics and never throw;
// close must consume the FILE* even when it reports failure. Not a public Config.
struct IoFunctions {
    using Write = std::size_t (*)(const void*, std::size_t, std::size_t, std::FILE*) noexcept;
    using Flush = int (*)(std::FILE*) noexcept;
    using Close = int (*)(std::FILE*) noexcept;

    static std::size_t systemWrite(const void* data, std::size_t size,
                                   std::size_t count, std::FILE* file) noexcept;
    static int systemFlush(std::FILE* file) noexcept;
    static int systemClose(std::FILE* file) noexcept;

    Write write = systemWrite;
    Flush flush = systemFlush;
    Close close = systemClose;
};

// Incremental JSON Lines archive. Paths are UTF-8; the output must not exist.
// JSON strings map original bytes to U+0000..U+00FF, including UTF-8/GBK bytes.
// Decode the JSON string and encode as Latin-1 to recover the original bytes.
// raw is the parser's physical line content, without its LF/CRLF terminator.
class ErrorLogWriter {
public:
    ErrorLogWriter(const std::string& input_path, const std::string& output_path,
                   IoFunctions io = {});
    ~ErrorLogWriter() noexcept;

    ErrorLogWriter(const ErrorLogWriter&) = delete;
    ErrorLogWriter& operator=(const ErrorLogWriter&) = delete;
    ErrorLogWriter(ErrorLogWriter&&) = delete;
    ErrorLogWriter& operator=(ErrorLogWriter&&) = delete;

    void write(std::uint64_t line, const std::string& raw,
               const std::vector<CellError>& errors);
    // Required for successful completion: validates buffered writes and close.
    // A successful close is idempotent. A failed writer remains failed.
    void close();
    std::uint64_t count() const noexcept { return count_; }

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept;
    };

    void writeBytes(const char* data, std::size_t length);
    void writeString(const std::string& value);
    void writeUnsigned(std::uint64_t value);
    void writeCode(ErrorCode code);
    [[noreturn]] void fail(const char* operation, int error_number);

    // Declared before file_ so the stdio buffer outlives fclose on every path.
    std::array<char, 64 * 1024> buffer_{};
    std::unique_ptr<std::FILE, FileCloser> file_;
    std::string output_path_;
    IoFunctions io_;
    std::uint64_t count_ = 0;
    int failure_code_ = 0;
};

} // namespace csvp::detail
