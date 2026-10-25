#include "csvp/parser.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace csvp {
namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

bool isContinuation(unsigned char c) {
    return c >= 0x80 && c <= 0xBF;
}

std::size_t utf8Length(const std::string& text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    const std::size_t remaining = text.size() - pos;
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) {
        return remaining >= 2 && isContinuation(static_cast<unsigned char>(text[pos + 1]))
                   ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (remaining < 3) return 0;
        const auto second = static_cast<unsigned char>(text[pos + 1]);
        const auto third = static_cast<unsigned char>(text[pos + 2]);
        if (!isContinuation(second) || !isContinuation(third)) return 0;
        // 排除过长编码和 UTF-16 surrogate 对应的非法 Unicode 值。
        if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0)) return 0;
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (remaining < 4) return 0;
        const auto second = static_cast<unsigned char>(text[pos + 1]);
        if (!isContinuation(second)
            || !isContinuation(static_cast<unsigned char>(text[pos + 2]))
            || !isContinuation(static_cast<unsigned char>(text[pos + 3]))) return 0;
        if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) return 0;
        return 4;
    }
    return 0;
}

} // namespace

CsvParser::CsvParser(const Config& cfg) : cfg_(cfg) {}

void CsvParser::FileCloser::operator()(std::FILE* file) const noexcept {
    if (file) std::fclose(file);
}

void CsvParser::setError(ErrorCode code, const std::string& message) {
    last_error_code_ = code;
    last_error_ = message;
}

bool CsvParser::countLine() {
    if (line_number_ == std::numeric_limits<std::uint64_t>::max()) {
        setError(ErrorCode::InternalError, "Physical line counter overflow");
        finished_ = true;
        return false;
    }
    ++line_number_;
    return true;
}

bool CsvParser::parse(const char* filepath) {
    file_.reset();
    header_.clear();
    raw_line_.clear();
    raw_line_truncated_ = false;
    last_error_.clear();
    last_error_code_ = ErrorCode::Ok;
    line_number_ = 0;
    bytes_read_ = 0;
    buffer_pos_ = buffer_end_ = 0;
    finished_ = false;

    std::string encoding = cfg_.encoding;
    for (char& c : encoding) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (encoding == "UTF-8" || encoding == "UTF8") {
        encoding_ = Encoding::Utf8;
    } else if (encoding == "GBK" || encoding == "CP936") {
        encoding_ = Encoding::Gbk;
    } else {
        setError(ErrorCode::EncodingError, "Unsupported CSV encoding: " + cfg_.encoding);
        finished_ = true;
        return false;
    }
    if (cfg_.delimiter == '\0' || cfg_.delimiter == '\r'
        || cfg_.delimiter == '\n' || cfg_.delimiter == '"'
        || static_cast<unsigned char>(cfg_.delimiter) >= 0x80) {
        setError(ErrorCode::ParseError, "Delimiter must be an ASCII character other than NUL, CR, LF or quote");
        finished_ = true;
        return false;
    }
    if (!filepath || !*filepath) {
        setError(ErrorCode::FileNotOpen, "CSV file path is empty");
        finished_ = true;
        return false;
    }

    std::FILE* opened = nullptr;
#ifdef _WIN32
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, nullptr, 0);
    if (length == 0) {
        setError(ErrorCode::EncodingError, "CSV file path is not valid UTF-8");
        finished_ = true;
        return false;
    }
    std::wstring wide_path(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1,
                            wide_path.data(), length) == 0) {
        setError(ErrorCode::EncodingError, "Failed to convert UTF-8 CSV file path");
        finished_ = true;
        return false;
    }
    _wfopen_s(&opened, wide_path.c_str(), L"rb");
#else
    opened = std::fopen(filepath, "rb");
#endif
    file_.reset(opened);
    if (!file_) {
        setError(ErrorCode::FileNotOpen, std::string("Failed to open file: ") + filepath);
        finished_ = true;
        return false;
    }

    if (cfg_.has_header) {
        Row header_row;
        const ReadStatus status = readNext(header_row);
        if (status != ReadStatus::Row) {
            if (status == ReadStatus::End) {
                setError(ErrorCode::ParseError, "CSV header is missing");
            }
            file_.reset();
            finished_ = true;
            return false;
        }
        header_.reserve(header_row.size());
        for (std::size_t i = 0; i < header_row.size(); ++i) header_.push_back(header_row[i]);
    }
    return true;
}

bool CsvParser::readRow(Row& row) {
    return readNext(row) == ReadStatus::Row;
}

CsvParser::ReadStatus CsvParser::readNext(Row& row) {
    row.clear();
    if (finished_) return ReadStatus::End;
    if (!file_) {
        setError(ErrorCode::FileNotOpen, "No CSV file opened");
        finished_ = true;
        row.setValid(false);
        return ReadStatus::Error;
    }

    for (;;) {
        const ReadStatus status = readLineFromFile();
        if (status != ReadStatus::Row) {
            if (status == ReadStatus::Error) row.setValid(false);
            return status;
        }
        std::size_t begin = 0;
        if (encoding_ == Encoding::Utf8 && line_number_ == 1 && raw_line_.size() >= 3
            && static_cast<unsigned char>(raw_line_[0]) == 0xEF
            && static_cast<unsigned char>(raw_line_[1]) == 0xBB
            && static_cast<unsigned char>(raw_line_[2]) == 0xBF) {
            begin = 3; // BOM 不属于第一个字段；原始行仍保留这些字节。
        }
        if (begin == raw_line_.size()) {
            if (finished_) return ReadStatus::End;
            continue; // 迭代跳过空行，不随连续空行增加调用栈。
        }
        if (!splitLineToFields(begin, row)) {
            row.clear();
            row.setValid(false);
            return ReadStatus::Error;
        }
        return ReadStatus::Row;
    }
}

CsvParser::ReadStatus CsvParser::readLineFromFile() {
    raw_line_.clear();
    raw_line_truncated_ = false;
    last_error_.clear();
    last_error_code_ = ErrorCode::Ok;
    if (finished_) return ReadStatus::End;

    std::uint64_t line_bytes = 0;
    char last_byte = '\0';
    bool ended_with_lf = false;
    // 多存一个字节，只用于允许长度恰好达上限的内容后面接 CRLF。
    const std::size_t retained_limit = cfg_.max_line_bytes == std::numeric_limits<std::size_t>::max()
                                          ? cfg_.max_line_bytes : cfg_.max_line_bytes + 1;

    for (;;) {
        if (buffer_pos_ == buffer_end_) {
            if (std::ferror(file_.get())) {
                if (line_bytes > 0 && !countLine()) return ReadStatus::Error;
                setError(ErrorCode::IOError, "Failed to read CSV file");
                finished_ = true;
                raw_line_truncated_ = line_bytes > cfg_.max_line_bytes;
                if (raw_line_.size() > cfg_.max_line_bytes) raw_line_.resize(cfg_.max_line_bytes);
                return ReadStatus::Error;
            }
            buffer_end_ = std::fread(input_buffer_.data(), 1, input_buffer_.size(), file_.get());
            buffer_pos_ = 0;
            if (buffer_end_ == 0) {
                if (std::ferror(file_.get())) {
                    if (line_bytes > 0 && !countLine()) return ReadStatus::Error;
                    setError(ErrorCode::IOError, "Failed to read CSV file");
                    finished_ = true;
                    raw_line_truncated_ = line_bytes > cfg_.max_line_bytes;
                    if (raw_line_.size() > cfg_.max_line_bytes) raw_line_.resize(cfg_.max_line_bytes);
                    return ReadStatus::Error;
                }
                finished_ = true;
                if (line_bytes == 0) return ReadStatus::End;
                break; // EOF 之前仍有数据，交付不带尾换行的末行。
            }
        }

        const char* begin = input_buffer_.data() + buffer_pos_;
        const std::size_t available = buffer_end_ - buffer_pos_;
        const char* newline = static_cast<const char*>(std::memchr(begin, '\n', available));
        const std::size_t chunk = newline ? static_cast<std::size_t>(newline - begin) : available;
        const std::size_t consumed = chunk + (newline ? 1 : 0);
        if (consumed > std::numeric_limits<std::uint64_t>::max() - bytes_read_) {
            setError(ErrorCode::InternalError, "Consumed byte counter overflow");
            finished_ = true;
            return ReadStatus::Error;
        }
        bytes_read_ += static_cast<std::uint64_t>(consumed);
        buffer_pos_ += consumed;
        line_bytes += static_cast<std::uint64_t>(chunk);
        if (chunk > 0) {
            last_byte = begin[chunk - 1];
            const std::size_t keep = std::min(chunk, retained_limit - raw_line_.size());
            raw_line_.append(begin, keep);
        }
        if (newline) {
            ended_with_lf = true;
            break;
        }
    }

    if (!countLine()) return ReadStatus::Error;
    // 只去掉作为 CRLF 终止符的 CR；EOF 前或引用字段内部的 CR 都保留。
    if (ended_with_lf && line_bytes > 0 && last_byte == '\r') {
        if (raw_line_.size() == line_bytes) raw_line_.pop_back();
        --line_bytes;
    }
    if (line_bytes > cfg_.max_line_bytes) {
        raw_line_truncated_ = true;
        if (raw_line_.size() > cfg_.max_line_bytes) raw_line_.resize(cfg_.max_line_bytes);
        setError(ErrorCode::ParseError, "CSV physical line exceeds max_line_bytes");
        return ReadStatus::Error;
    }
    return ReadStatus::Row;
}

std::size_t CsvParser::characterLength(std::size_t pos) const {
    if (encoding_ == Encoding::Utf8) return utf8Length(raw_line_, pos);
    const auto lead = static_cast<unsigned char>(raw_line_[pos]);
    if (lead <= 0x80) return 1; // ASCII，以及 CP936 的单字节欧元符号。
    if (lead > 0xFE || raw_line_.size() - pos < 2) return 0;
    const auto trail = static_cast<unsigned char>(raw_line_[pos + 1]);
    return ((trail >= 0x40 && trail <= 0x7E) || (trail >= 0x80 && trail <= 0xFE)) ? 2 : 0;
}

bool CsvParser::splitLineToFields(std::size_t begin, Row& row) {
    enum class State { Start, Unquoted, Quoted, AfterQuote };
    State state = State::Start;
    std::string field;
    field.reserve(256);

    const auto emit = [&](bool trim) {
        if (trim && cfg_.trim_spaces) {
            while (!field.empty() && isSpace(field.back())) field.pop_back();
        }
        row.append(field);
        field.clear();
    };

    for (std::size_t pos = begin; pos < raw_line_.size();) {
        const char c = raw_line_[pos];
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\0') {
            setError(ErrorCode::ParseError, "NUL byte is not allowed in CSV fields");
            return false;
        }
        if (byte >= 0x80) {
            const std::size_t length = characterLength(pos);
            if (length == 0) {
                setError(ErrorCode::EncodingError, "Invalid " + cfg_.encoding + " byte sequence in CSV row");
                return false;
            }
            if (state == State::AfterQuote) {
                setError(ErrorCode::ParseError, "Unexpected character after quoted field");
                return false;
            }
            if (state == State::Start) state = State::Unquoted;
            field.append(raw_line_, pos, length);
            pos += length;
            continue;
        }

        switch (state) {
        case State::Start:
            if (c == cfg_.delimiter) {
                emit(true);
            } else if (c == '"') {
                state = State::Quoted;
            } else if (!(cfg_.trim_spaces && isSpace(c))) {
                field += c;
                state = State::Unquoted;
            }
            ++pos;
            break;
        case State::Unquoted:
            if (c == cfg_.delimiter) {
                emit(true);
                state = State::Start;
            } else if (c == '"') {
                setError(ErrorCode::ParseError, "Quote inside an unquoted field");
                return false;
            } else {
                field += c;
            }
            ++pos;
            break;
        case State::Quoted:
            if (c == '"') {
                if (pos + 1 < raw_line_.size() && raw_line_[pos + 1] == '"') {
                    field += '"';
                    pos += 2;
                } else {
                    state = State::AfterQuote;
                    ++pos;
                }
            } else {
                field += c;
                ++pos;
            }
            break;
        case State::AfterQuote:
            if (c == cfg_.delimiter) {
                emit(false);
                state = State::Start;
            } else if (!(cfg_.trim_spaces && isSpace(c))) {
                setError(ErrorCode::ParseError, "Unexpected character after quoted field");
                return false;
            }
            ++pos;
            break;
        }
    }
    if (state == State::Quoted) {
        setError(ErrorCode::ParseError, "Unclosed quote; multiline CSV fields are not supported");
        return false;
    }
    emit(state != State::AfterQuote);
    return true;
}

} // namespace csvp
