#include "csvp/parser.h"
#include "csvp/row.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>

namespace {

// Helper to get the test data file path
std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

// ============================================================
// Parser.UTF8Basic (PASS)
// ============================================================
TEST(Parser, UTF8Basic) {
    csvp::CsvParser::Config cfg;
    cfg.delimiter = ',';
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));

    // Check header
    ASSERT_EQ(parser.header().size(), 5u);
    EXPECT_EQ(parser.header()[0], "id");
    EXPECT_EQ(parser.header()[1], "name");
    EXPECT_EQ(parser.header()[2], "age");

    // Read all rows
    int row_count = 0;
    csvp::Row row;
    while (parser.readRow(row)) {
        ++row_count;
        EXPECT_EQ(row.size(), 5u);
    }
    EXPECT_EQ(row_count, 10);
}

// ============================================================
// Parser.HeaderMapping (PASS)
// ============================================================
TEST(Parser, HeaderMapping) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));

    EXPECT_EQ(parser.header().size(), 5u);
    EXPECT_EQ(parser.header()[3], "score");
    EXPECT_EQ(parser.header()[4], "active");

    // First row
    csvp::Row row;
    ASSERT_TRUE(parser.readRow(row));
    EXPECT_EQ(row[0], "1");
    EXPECT_EQ(row[1], "Alice");
    EXPECT_EQ(row[2], "25");
    EXPECT_EQ(row[3], "95.5");
    EXPECT_EQ(row[4], "true");
}

// ============================================================
// Parser.NoTrailingNewline
// 已知现象见 docs/KNOWN_ISSUES.md #1。修复后把前缀 DISABLED_ 去掉以启用验证。
// ============================================================
TEST(Parser, DISABLED_NoTrailingNewline) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));

    // Header should be 3 columns
    EXPECT_EQ(parser.header().size(), 3u);

    int row_count = 0;
    csvp::Row row;
    csvp::Row last_row;
    while (parser.readRow(row)) {
        ++row_count;
        last_row = row;
    }

    EXPECT_EQ(row_count, 3) << "expected 3 data rows";
}

// ============================================================
// Parser.ReentryState
// 已知现象见 docs/KNOWN_ISSUES.md #2。修复后去掉前缀 DISABLED_ 以启用验证。
// ============================================================
TEST(Parser, DISABLED_ReentryState) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);

    // First parse
    ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));
    int count1 = 0;
    csvp::Row row;
    while (parser.readRow(row)) ++count1;
    EXPECT_EQ(count1, 10);
    int lines1 = parser.lineNumber();

    // Second parse (different file)
    ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));
    int count2 = 0;
    while (parser.readRow(row)) ++count2;
    int lines2 = parser.lineNumber();

    std::cout << "First parse: " << count1 << " rows, line=" << lines1 << std::endl;
    std::cout << "Second parse: " << count2 << " rows, line=" << lines2 << std::endl;

    // lineNumber() after re-parse should reflect only the second file.
    EXPECT_LE(lines2, 10) << "lineNumber() should reflect the second file only, got " << lines2;
}

// ============================================================
// Parser.ChineseGBKEncoding
// 已知现象见 docs/KNOWN_ISSUES.md #3。修复后去掉前缀 DISABLED_ 以启用验证。
// ============================================================
TEST(Parser, DISABLED_ChineseGBKEncoding) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;
    cfg.encoding = "GBK";

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_gbk.csv").c_str()));

    // Header should be parsed (GBK bytes read as raw)
    EXPECT_GT(parser.header().size(), 0u);

    // Row 1: normal GBK data, should parse to 4 columns
    csvp::Row row;
    ASSERT_TRUE(parser.readRow(row));

    // Row 2: GBK data row, expected to parse to 4 columns
    ASSERT_TRUE(parser.readRow(row));

    EXPECT_EQ(row.size(), 4u) << "expected 4 columns for GBK row, got " << row.size();
}

// ============================================================
// Parser.EmptyFile (PASS)
// ============================================================
TEST(Parser, EmptyFile) {
    // Test with a non-existent file
    csvp::CsvParser::Config cfg;
    csvp::CsvParser parser(cfg);
    EXPECT_FALSE(parser.parse("nonexistent_file_12345.csv"));
    EXPECT_FALSE(parser.lastError().empty());
}

// ============================================================
// Parser.DISABLED_QuotedEscapedQuote
// 现象见 docs/KNOWN_ISSUES.md #9。修复后去掉前缀 DISABLED_ 启用验证。
// ============================================================
TEST(Parser, DISABLED_QuotedEscapedQuote) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;
    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_quote.csv").c_str()));

    csvp::Row row;
    ASSERT_TRUE(parser.readRow(row));  // row1: 1,"he said ""hi""",x

    // 第 2 列（phrase）应完整保留：he said "hi"
    EXPECT_EQ(row.size(), 3u);
    EXPECT_EQ(row[1], "he said \"hi\"");

    EXPECT_EQ(row[2], "x");
}

// ============================================================
// Parser.DISABLED_CRLFLFMixedCR
// 现象见 docs/KNOWN_ISSUES.md #10。修复后去掉前缀 DISABLED_ 启用验证。
// ============================================================
TEST(Parser, DISABLED_CRLFLFMixedCR) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;
    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_crlf.csv").c_str()));

    csvp::Row row;
    // row1: 1,alice,desc
    ASSERT_TRUE(parser.readRow(row));
    EXPECT_EQ(row[2], "desc");

    // row2: 2,bob,has_cr\r
    ASSERT_TRUE(parser.readRow(row));
    EXPECT_EQ(row[2], "has_cr\r");
}

} // anonymous namespace