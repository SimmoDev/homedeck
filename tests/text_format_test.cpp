#include "ui/text_format.h"

#include <gtest/gtest.h>

// Every case below is a HarmonyControlGroup/HarmonyCommand name pulled
// from the reference hub (see DevicesScreen's own comment) - not
// guessed shapes.

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

TEST(SplitCamelCaseTest, HandlesASingleWordOrLetter) {
    EXPECT_EQ(homedeck::SplitCamelCase("Mute"), "Mute");
    EXPECT_EQ(homedeck::SplitCamelCase("A"), "A");
}

TEST(SplitCamelCaseTest, HandlesEmptyInput) { EXPECT_EQ(homedeck::SplitCamelCase(""), ""); }
