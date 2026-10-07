#include "ui/lazy_load.h"

#include <gtest/gtest.h>

TEST(LazyLoadTest, LoadsOnFirstShowOnceConnected) {
    homedeck::LazyLoad lazy;
    EXPECT_FALSE(lazy.OnShown(/*connected=*/false));
    EXPECT_TRUE(lazy.OnShown(/*connected=*/true));
    EXPECT_FALSE(lazy.OnShown(/*connected=*/true)) << "data is current; showing again must not reload";
}

TEST(LazyLoadTest, ConnectingWhileHiddenDefersTheLoadToTheNextShow) {
    homedeck::LazyLoad lazy;
    EXPECT_FALSE(lazy.OnConnected(/*screen_active=*/false));
    EXPECT_TRUE(lazy.OnShown(/*connected=*/true));
}

TEST(LazyLoadTest, ConnectingWhileShowingLoadsAtOnce) {
    homedeck::LazyLoad lazy;
    EXPECT_TRUE(lazy.OnConnected(/*screen_active=*/true));
    EXPECT_FALSE(lazy.OnShown(/*connected=*/true));
}

TEST(LazyLoadTest, EachReconnectMakesTheDataStaleAgain) {
    homedeck::LazyLoad lazy;
    ASSERT_TRUE(lazy.OnShown(true));
    EXPECT_FALSE(lazy.OnShown(true));
    EXPECT_FALSE(lazy.OnConnected(false));
    EXPECT_TRUE(lazy.OnShown(true));
}
