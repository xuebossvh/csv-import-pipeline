#include "error_log.h"

#include <cerrno>
#include <charconv>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace csvp::detail {
namespace {

namespace fs = std::filesystem;

bool equalNormalizedPaths(const fs::path& first, const fs::path& second) {
#ifdef _WIN32
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return first == second;
#endif
}

void validateOutputPath(const std::string& input_path, const std::string& output_path) {
    if (output_path.empty()) throw std::invalid_argument("Error log path is empty");
    if (input_path.find('\0') != std::string::npos || output_path.find('\0') != std::string::npos) {
        throw std::invalid_argument("Error log paths must not contain NUL bytes");
    }

    const fs::path output = fs::u8path(output_path);
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(output, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory
        && status_error != std::errc::not_a_directory) {
        throw fs::filesystem_error("Cannot inspect error log path", output, status_error);
    }
    if (!status_error && fs::exists(status)) {
        throw std::runtime_error("Error log output already exists: " + output_path);
    }

    // A nonexistent/empty input may itself be the import error being archived.
    if (input_path.empty()) return;
    const fs::path input = fs::u8path(input_path);
    const fs::path input_normal = fs::weakly_canonical(fs::absolute(input));
    const fs::path output_normal = fs::weakly_canonical(fs::absolute(output));
    std::error_code equivalent_error;
    const bool equivalent = fs::equivalent(input, output, equivalent_error);
    if (equalNormalizedPaths(input_normal, output_normal) || (!equivalent_error && equivalent)) {
        throw std::invalid_argument("Error log output must differ from the CSV input");
    }
}

} // namespace

std::size_t IoFunctions::systemWrite(const void* data, std::size_t size,
                                    std::size_t count, std::FILE* file) noexcept {
    return std::fwrite(data, size, count, file);
}

int IoFunctions::systemFlush(std::FILE* file) noexcept {
    return std::fflush(file);
}

int IoFunctions::systemClose(std::FILE* file) noexcept {
    return std::fclose(file);
}

void ErrorLogWriter::FileCloser::operator()(std::FILE* file) const noexcept {
    if (file) std::fclose(file);
}

ErrorLogWriter::ErrorLogWriter(const std::string& input_path, const std::string& output_path,
                               IoFunctions io)
    : output_path_(output_path), io_(io) {
    if (!io_.write || !io_.flush || !io_.close) {
        throw std::invalid_argument("Error log I/O hooks must not be null");
    }
    validateOutputPath(input_path, output_path);
    const fs::path output = fs::u8path(output_path);
    // The preflight check improves diagnostics; exclusive creation below is the
    // actual race-safe guarantee that an existing file is never truncated.
#ifdef _WIN32
    const int descriptor = _wopen(output.c_str(),
                                  _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                                  _S_IREAD | _S_IWRITE);
    if (descriptor == -1) fail("create", errno);
    std::FILE* stream = _fdopen(descriptor, "wb");
    if (!stream) {
        const int open_error = errno;
        _close(descriptor);
        fail("open", open_error);
    }
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(output.c_str(), flags, 0666);
    if (descriptor == -1) fail("create", errno);
    std::FILE* stream = ::fdopen(descriptor, "wb");
    if (!stream) {
        const int open_error = errno;
        ::close(descriptor);
        fail("open", open_error);
    }
#endif
    file_.reset(stream);
    if (std::setvbuf(file_.get(), buffer_.data(), _IOFBF, buffer_.size()) != 0) {
        fail("configure buffer for", EIO);
    }
}

ErrorLogWriter::~ErrorLogWriter() noexcept {
    // Only a fallback; always real fclose, without injected callbacks. The normal
    // path calls close() and observes I/O failures before releasing the resource.
    file_.reset();
}

[[noreturn]] void ErrorLogWriter::fail(const char* operation, int error_number) {
    failure_code_ = error_number == 0 ? EIO : error_number;
    throw std::system_error(failure_code_, std::generic_category(),
                            std::string("Cannot ") + operation + " error log: " + output_path_);
}

void ErrorLogWriter::writeBytes(const char* data, std::size_t length) {
    if (length == 0) return;
    errno = 0;
    const std::size_t written = io_.write(data, 1, length, file_.get());
    if (written != length || std::ferror(file_.get())) fail("write", errno);
}

void ErrorLogWriter::writeUnsigned(std::uint64_t value) {
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    if (result.ec != std::errc{}) fail("format a number for", EIO);
    writeBytes(digits, static_cast<std::size_t>(result.ptr - digits));
}

void ErrorLogWriter::writeCode(ErrorCode code) {
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof(digits), static_cast<int>(code));
    if (result.ec != std::errc{}) fail("format an error code for", EIO);
    writeBytes(digits, static_cast<std::size_t>(result.ptr - digits));
}

void ErrorLogWriter::writeString(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    writeBytes("\"", 1);
    std::size_t run = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\') continue;

        writeBytes(value.data() + run, i - run);
        char escape[6] = {'\\', '\0', '\0', '\0', '\0', '\0'};
        std::size_t length = 2;
        switch (byte) {
        case '"': escape[1] = '"'; break;
        case '\\': escape[1] = '\\'; break;
        case '\b': escape[1] = 'b'; break;
        case '\f': escape[1] = 'f'; break;
        case '\n': escape[1] = 'n'; break;
        case '\r': escape[1] = 'r'; break;
        case '\t': escape[1] = 't'; break;
        default:
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = hex[byte >> 4];
            escape[5] = hex[byte & 0x0F];
            length = 6;
            break;
        }
        writeBytes(escape, length);
        run = i + 1;
    }
    writeBytes(value.data() + run, value.size() - run);
    writeBytes("\"", 1);
}

void ErrorLogWriter::write(std::uint64_t line, const std::string& raw,
                          const std::vector<CellError>& errors) {
    if (failure_code_ != 0) fail("write to failed", failure_code_);
    if (!file_) throw std::logic_error("Error log is already closed");
    if (count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Error log record counter overflow");
    }

    writeBytes("{\"line_number\":", 15);
    writeUnsigned(line);
    writeBytes(",\"raw_line\":", 12);
    writeString(raw);
    writeBytes(",\"errors\":[", 11);
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i != 0) writeBytes(",", 1);
        const CellError& error = errors[i];
        writeBytes("{\"column_index\":", 16);
        writeUnsigned(static_cast<std::uint64_t>(error.column_index));
        writeBytes(",\"column_name\":", 15);
        writeString(error.column_name);
        writeBytes(",\"code\":", 8);
        writeCode(error.code);
        writeBytes(",\"message\":", 11);
        writeString(error.message);
        writeBytes(",\"raw_value\":", 13);
        writeString(error.raw_value);
        writeBytes("}", 1);
    }
    writeBytes("]}\n", 3);
    ++count_;
}

void ErrorLogWriter::close() {
    if (!file_) {
        if (failure_code_ != 0) fail("close failed", failure_code_);
        return;
    }

    // Release ownership before fclose: even a failed fclose consumes the FILE*.
    std::FILE* stream = file_.release();
    int close_error = failure_code_;
    errno = 0;
    if (io_.flush(stream) != 0 && close_error == 0) close_error = errno == 0 ? EIO : errno;
    errno = 0;
    if (io_.close(stream) != 0 && close_error == 0) close_error = errno == 0 ? EIO : errno;
    if (close_error != 0) fail("close", close_error);
}

} // namespace csvp::detail
