#include "csvp/validator.h"
#include "csvp/row.h"

#include <gtest/gtest.h>
#include <vector>
#include <string>

namespace {

// ============================================================
// Validator.TypeCheckString (PASS)
// ============================================================
TEST(Validator, TypeCheckString) {
    csvp::FieldValidator v;
    v.addRule({"name", csvp::FieldType::String, true});

    csvp::Row row({"Alice"});
    auto errors = v.validate(row);
    EXPECT_TRUE(errors.empty());
}

// ============================================================
// Validator.TypeCheckInt (PASS)
// ============================================================
TEST(Validator, TypeCheckInt) {
    csvp::FieldValidator v;
    v.addRule({"age", csvp::FieldType::Int, true});

    // Valid int
    csvp::Row row1({"25"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Invalid int
    csvp::Row row2({"N/A"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::TypeConversionError);
}

// ============================================================
// Validator.TypeCheckDouble (PASS)
// ============================================================
TEST(Validator, TypeCheckDouble) {
    csvp::FieldValidator v;
    v.addRule({"score", csvp::FieldType::Double, true});

    // Valid double
    csvp::Row row1({"95.5"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Invalid double
    csvp::Row row2({"abc"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::TypeConversionError);
}

// ============================================================
// Validator.RequiredField (PASS)
// ============================================================
TEST(Validator, RequiredField) {
    csvp::FieldValidator v;
    v.addRule({"name", csvp::FieldType::String, true});

    // Empty required field
    csvp::Row row1({""});
    auto errors1 = v.validate(row1);
    EXPECT_FALSE(errors1.empty());
    EXPECT_EQ(errors1[0].code, csvp::ErrorCode::RequiredFieldMissing);
}

// ============================================================
// Validator.RangeCheck (PASS)
// ============================================================
TEST(Validator, RangeCheck) {
    csvp::FieldValidator v;
    csvp::FieldRule rule;
    rule.name = "age";
    rule.type = csvp::FieldType::Int;
    rule.has_range = true;
    rule.min_val = 0;
    rule.max_val = 150;
    v.addRule(rule);

    // Within range
    csvp::Row row1({"25"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Out of range
    csvp::Row row2({"200"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::RangeError);
}

// ============================================================
// Validator.RegexCheck (PASS)
// ============================================================
TEST(Validator, RegexCheck) {
    csvp::FieldValidator v;
    csvp::FieldRule rule;
    rule.name = "email";
    rule.type = csvp::FieldType::String;
    rule.regex = R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})";
    v.addRule(rule);

    csvp::Row row1({"alice@example.com"});
    EXPECT_TRUE(v.validate(row1).empty());

    csvp::Row row2({"not-an-email"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::RegexMismatch);
}

// ============================================================
// Validator.MultipleRules (PASS)
// ============================================================
TEST(Validator, MultipleRules) {
    csvp::FieldValidator v;
    v.addRule({"id", csvp::FieldType::Int, true});
    v.addRule({"name", csvp::FieldType::String, true});
    v.addRule({"score", csvp::FieldType::Double, false});

    csvp::Row row1({"1", "Alice", "95.5"});
    EXPECT_TRUE(v.validate(row1).empty());

    csvp::Row row2({"abc", "Bob", "bad"});
    auto errors2 = v.validate(row2);
    EXPECT_EQ(errors2.size(), 2u);
}

} // anonymous namespace