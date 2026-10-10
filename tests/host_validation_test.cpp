#include "core/host_validation.h"

#include <gtest/gtest.h>

using homedeck::HasUnsafeHostChars;

TEST(HasUnsafeHostCharsTest, AcceptsPlainHostnamesAndIpv4) {
    EXPECT_FALSE(HasUnsafeHostChars("kodi", false));
    EXPECT_FALSE(HasUnsafeHostChars("living-room.local", false));
    EXPECT_FALSE(HasUnsafeHostChars("10.0.30.20", false));
}

TEST(HasUnsafeHostCharsTest, RejectsASchemePrefix) {
    EXPECT_TRUE(HasUnsafeHostChars("http://kodi", false));
    EXPECT_TRUE(HasUnsafeHostChars("ws://10.0.0.1", true));
}

TEST(HasUnsafeHostCharsTest, RejectsWhitespaceControlBytesAndNonAscii) {
    EXPECT_TRUE(HasUnsafeHostChars("kodi box", false));
    EXPECT_TRUE(HasUnsafeHostChars("kodi\t", false));
    EXPECT_TRUE(HasUnsafeHostChars(std::string("kodi\x01", 5), false));
    EXPECT_TRUE(HasUnsafeHostChars("kodi\x7f", false));
    EXPECT_TRUE(HasUnsafeHostChars("k\xc3\xb6" "di", false));
    EXPECT_TRUE(HasUnsafeHostChars("kodi\xc2\xa0", true));  // no-break space
}

TEST(HasUnsafeHostCharsTest, RejectsCharactersThatChangeTheUrlAuthority) {
    EXPECT_TRUE(HasUnsafeHostChars("kodi/path", true));
    EXPECT_TRUE(HasUnsafeHostChars("kodi#frag", true));
    EXPECT_TRUE(HasUnsafeHostChars("kodi?query", true));
    EXPECT_TRUE(HasUnsafeHostChars("user@kodi", true));
}

TEST(HasUnsafeHostCharsTest, AColonIsAllowedOnlyWhenAsked) {
    EXPECT_TRUE(HasUnsafeHostChars("::1", false));
    EXPECT_FALSE(HasUnsafeHostChars("fe80::1", true));
}

TEST(HasUnsafeHostCharsTest, AnEmptyStringHasNothingUnsafe) {
    EXPECT_FALSE(HasUnsafeHostChars("", false));
}

TEST(IsValidHostnameLabelTest, AcceptsLettersDigitsAndInnerHyphens) {
    EXPECT_TRUE(homedeck::IsValidHostnameLabel("homedeck"));
    EXPECT_TRUE(homedeck::IsValidHostnameLabel("living-room-2"));
    EXPECT_TRUE(homedeck::IsValidHostnameLabel(std::string(63, 'a')));
}

TEST(IsValidHostnameLabelTest, RejectsEmptyOverlongAndMisplacedHyphens) {
    EXPECT_FALSE(homedeck::IsValidHostnameLabel(""));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel(std::string(64, 'a')));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("-room"));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("room-"));
}

TEST(IsValidHostnameLabelTest, RejectsEverythingElse) {
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("living room"));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("room.local"));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("under_score"));
    EXPECT_FALSE(homedeck::IsValidHostnameLabel("k\xc3\xb6di"));
}
