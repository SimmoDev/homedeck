#include "platform/websocket_message_assembler.h"

#include <gtest/gtest.h>

#include <string>

namespace homedeck {
namespace {

using Result = WebSocketMessageAssembler::Result;

Result AddText(WebSocketMessageAssembler& a, const std::string& text, bool fin = true, bool frame_complete = true) {
    return a.Add(text.data(), text.size(), fin, frame_complete);
}

TEST(WebSocketMessageAssemblerTest, ASingleCompleteFrameIsQueued) {
    WebSocketMessageAssembler a;
    EXPECT_EQ(AddText(a, "hello"), Result::kComplete);
    EXPECT_EQ(a.Pop(), "hello");
    EXPECT_FALSE(a.Pop().has_value());
}

TEST(WebSocketMessageAssemblerTest, OneFrameArrivingInChunksCompletesOnItsLastChunk) {
    WebSocketMessageAssembler a;
    EXPECT_EQ(AddText(a, "ab", true, false), Result::kPartial);
    EXPECT_FALSE(a.HasMessage());
    EXPECT_EQ(AddText(a, "cd", true, true), Result::kComplete);
    EXPECT_EQ(a.Pop(), "abcd");
}

TEST(WebSocketMessageAssemblerTest, AMessageSpanningFramesCompletesOnlyOnTheFinFrame) {
    WebSocketMessageAssembler a;
    EXPECT_EQ(AddText(a, "one-", false, true), Result::kPartial);
    EXPECT_EQ(AddText(a, "two-", false, true), Result::kPartial);
    EXPECT_EQ(AddText(a, "three", true, true), Result::kComplete);
    EXPECT_EQ(a.Pop(), "one-two-three");
}

TEST(WebSocketMessageAssemblerTest, AMessageOverTheBoundIsDiscardedAndTheNextOneStartsClean) {
    WebSocketMessageAssembler a;
    const std::string big(kMaxWebSocketMessageBytes, 'x');
    EXPECT_EQ(AddText(a, big, false, true), Result::kPartial);
    EXPECT_EQ(AddText(a, "y", false, true), Result::kOversized);
    EXPECT_EQ(AddText(a, "next"), Result::kComplete);
    EXPECT_EQ(a.Pop(), "next");
}

TEST(WebSocketMessageAssemblerTest, AMessageExactlyAtTheBoundIsAccepted) {
    WebSocketMessageAssembler a;
    const std::string big(kMaxWebSocketMessageBytes, 'x');
    EXPECT_EQ(AddText(a, big), Result::kComplete);
    EXPECT_EQ(a.Pop()->size(), kMaxWebSocketMessageBytes);
}

TEST(WebSocketMessageAssemblerTest, AFullQueueDropsTheOldestMessages) {
    WebSocketMessageAssembler a;
    for (size_t i = 0; i < kMaxQueuedWebSocketMessages + 3; ++i) {
        ASSERT_EQ(AddText(a, std::to_string(i)), Result::kComplete);
    }
    EXPECT_EQ(a.Pop(), "3");
    size_t remaining = 1;
    while (a.Pop().has_value()) {
        ++remaining;
    }
    EXPECT_EQ(remaining, kMaxQueuedWebSocketMessages);
}

TEST(WebSocketMessageAssemblerTest, ClearDropsQueuedAndInProgressData) {
    WebSocketMessageAssembler a;
    AddText(a, "queued");
    AddText(a, "partial", false, true);
    a.Clear();
    EXPECT_FALSE(a.HasMessage());
    EXPECT_EQ(AddText(a, "fresh"), Result::kComplete);
    EXPECT_EQ(a.Pop(), "fresh");
}

}  // namespace
}  // namespace homedeck
