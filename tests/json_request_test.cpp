#include "core/json_request.h"

#include <gtest/gtest.h>

using homedeck::ExceedsJsonNestingDepth;
using homedeck::TryParseJsonObject;

namespace {

std::string Nested(int levels) { return std::string(levels, '[') + std::string(levels, ']'); }

}  // namespace

TEST(ExceedsJsonNestingDepthTest, AllowsDepthUpToTheLimitAndRejectsBeyond) {
    EXPECT_FALSE(ExceedsJsonNestingDepth(Nested(32)));
    EXPECT_TRUE(ExceedsJsonNestingDepth(Nested(33)));
    EXPECT_TRUE(ExceedsJsonNestingDepth(Nested(5), /*max_depth=*/4));
}

TEST(ExceedsJsonNestingDepthTest, IgnoresBracketsInsideStrings) {
    EXPECT_FALSE(ExceedsJsonNestingDepth(R"({"a":"[[[[[[[[[[","b":"{{{{{{{{{{"})", 2));
}

TEST(ExceedsJsonNestingDepthTest, AnEscapedQuoteDoesNotEndTheString) {
    // The string is  \"[[[[  inside quotes - still a string, so depth stays at 1.
    EXPECT_FALSE(ExceedsJsonNestingDepth(R"({"a":"\"[[[["})", 2));
}

TEST(TryParseJsonObjectTest, ReturnsTheObject) {
    auto parsed = TryParseJsonObject(R"({"a":1})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["a"], 1);
}

TEST(TryParseJsonObjectTest, RejectsMalformedNonObjectAndTooDeepBodies) {
    EXPECT_FALSE(TryParseJsonObject("{not json").has_value());
    EXPECT_FALSE(TryParseJsonObject("[1,2]").has_value());
    EXPECT_FALSE(TryParseJsonObject("42").has_value());
    EXPECT_FALSE(TryParseJsonObject(R"({"a":)" + Nested(40) + "}").has_value());
}
