#include "core/url_codec.h"

#include <gtest/gtest.h>

TEST(UrlDecodeTest, LeavesPlainAlphanumericTextUnchanged) {
    EXPECT_EQ(homedeck::UrlDecode("HomeDeck123"), "HomeDeck123");
}

TEST(UrlDecodeTest, DecodesPlusAsSpace) {
    EXPECT_EQ(homedeck::UrlDecode("Living+Room+5G"), "Living Room 5G");
}

TEST(UrlDecodeTest, DecodesPercentEncodedBytes) {
    // "Chris's Wifi!" as submitted by a browser form.
    EXPECT_EQ(homedeck::UrlDecode("Chris%27s+Wifi%21"), "Chris's Wifi!");
}

TEST(UrlDecodeTest, LeavesATrailingIncompletePercentEscapeLiteral) {
    // Malformed input (truncated escape) shouldn't read past the end of
    // the string or crash - just pass the '%' through unchanged.
    EXPECT_EQ(homedeck::UrlDecode("abc%2"), "abc%2");
    EXPECT_EQ(homedeck::UrlDecode("abc%"), "abc%");
}

TEST(ParseFormFieldTest, ExtractsAndDecodesTheNamedField) {
    auto value = homedeck::ParseFormField("ssid=Living+Room+5G&password=hunter2", "ssid");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "Living Room 5G");
}

TEST(ParseFormFieldTest, ExtractsTheSecondFieldNotJustTheFirst) {
    auto value = homedeck::ParseFormField("ssid=MyNetwork&password=P%40ssw0rd%21", "password");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "P@ssw0rd!");
}

TEST(ParseFormFieldTest, ReturnsNulloptWhenTheKeyIsAbsent) {
    EXPECT_FALSE(homedeck::ParseFormField("ssid=MyNetwork", "password").has_value());
}

TEST(ParseFormFieldTest, ReturnsAnEmptyStringForAPresentButEmptyValue) {
    auto value = homedeck::ParseFormField("ssid=MyNetwork&password=", "password");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "");
}
