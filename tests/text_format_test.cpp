#include "ui/text_format.h"

#include <gtest/gtest.h>

// Every case below is a HarmonyControlGroup/HarmonyCommand name pulled
// from the reference hub (see DevicesScreen's own comment) - not
// guessed shapes - except SplitsAtADigitToUpperBoundary, which verifies
// a documented rule (text_format.h's own header comment) not observed
// on any command name from that hub.

TEST(SplitCamelCaseTest, SplitsAtALowerToUpperBoundary) {
    EXPECT_EQ(homedeck::SplitCamelCase("RightBumper"), "Right Bumper");
    EXPECT_EQ(homedeck::SplitCamelCase("PowerToggle"), "Power Toggle");
    EXPECT_EQ(homedeck::SplitCamelCase("FastForward"), "Fast Forward");
}

TEST(SplitCamelCaseTest, KeepsAnAcronymRunTogether) {
    // "TVShows": the acronym boundary (upper-upper-lower) falls before
    // the 'S', not between every letter of "TV".
    EXPECT_EQ(homedeck::SplitCamelCase("TVShows"), "TV Shows");
    EXPECT_EQ(homedeck::SplitCamelCase("NavigationDSTB"), "Navigation DSTB");
}

TEST(SplitCamelCaseTest, IsANoOpOnAlreadySpacedText) {
    // The hub's own `label` field is already spaced for some commands
    // (unlike the "Miscellaneous" group's raw PascalCase names) -
    // applying this unconditionally must not double-space or otherwise
    // change it.
    EXPECT_EQ(homedeck::SplitCamelCase("Volume Down"), "Volume Down");
}

TEST(SplitCamelCaseTest, KeepsATrailingDigitAttached) {
    EXPECT_EQ(homedeck::SplitCamelCase("GameType1"), "Game Type1");
}

TEST(SplitCamelCaseTest, SplitsAtADigitToUpperBoundary) {
    // A digit counts as the "lower" side of the lower-to-upper boundary
    // rule too (see this function's own header comment) - the digit
    // itself stays attached to what precedes it, same as
    // KeepsATrailingDigitAttached above, but a space is still inserted
    // before the uppercase letter that follows it.
    EXPECT_EQ(homedeck::SplitCamelCase("Mode3D"), "Mode3 D");
    // The digit-to-upper boundary rule applies independently of the
    // acronym-run rule (KeepsAnAcronymRunTogether above) - a digit
    // immediately before an acronym run still splits before the run's
    // first letter, and the acronym rule then splits again at the run's
    // own upper-to-lower boundary, same as if a lowercase letter preceded
    // the run instead of a digit.
    EXPECT_EQ(homedeck::SplitCamelCase("3DMode"), "3 D Mode");
}

TEST(SplitCamelCaseTest, HandlesASingleWordOrLetter) {
    EXPECT_EQ(homedeck::SplitCamelCase("Mute"), "Mute");
    EXPECT_EQ(homedeck::SplitCamelCase("A"), "A");
}

TEST(SplitCamelCaseTest, HandlesEmptyInput) { EXPECT_EQ(homedeck::SplitCamelCase(""), ""); }
