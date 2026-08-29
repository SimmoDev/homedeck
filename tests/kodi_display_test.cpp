#include "ui/kodi_display.h"

#include <gtest/gtest.h>

namespace {

homedeck::KodiSnapshot Connected(homedeck::KodiNowPlaying now_playing) {
    homedeck::KodiSnapshot s;
    s.state = homedeck::KodiConnectionState::kConnected;
    s.now_playing = std::move(now_playing);
    return s;
}

homedeck::KodiNowPlaying Episode() {
    homedeck::KodiNowPlaying np;
    np.playback = homedeck::KodiPlaybackState::kPlaying;
    np.show_title = "The Show";
    np.season = 3;
    np.episode = 7;
    np.title = "The One With The Test";
    np.media_type = "episode";
    return np;
}

}  // namespace

TEST(KodiWidgetLineTest, ReportsEachNonConnectedState) {
    homedeck::KodiSnapshot s;
    s.state = homedeck::KodiConnectionState::kConnecting;
    EXPECT_EQ(homedeck::KodiWidgetLine(s), "Connecting...");
    s.state = homedeck::KodiConnectionState::kError;
    EXPECT_EQ(homedeck::KodiWidgetLine(s), "Not reachable");
    s.state = homedeck::KodiConnectionState::kDisconnected;
    EXPECT_EQ(homedeck::KodiWidgetLine(s), "Not configured");
    s.discovered = {{"Living Room", "10.0.0.1", "a"}, {"Bedroom", "10.0.0.2", "b"}};
    EXPECT_EQ(homedeck::KodiWidgetLine(s), "Choose a Kodi in settings");
}

TEST(KodiWidgetLineTest, ReportsPlaybackWhenConnected) {
    EXPECT_EQ(homedeck::KodiWidgetLine(Connected({})), "Idle");

    homedeck::KodiNowPlaying playing = Episode();
    EXPECT_EQ(homedeck::KodiWidgetLine(Connected(playing)), "Playing: The Show");

    homedeck::KodiNowPlaying paused_movie;
    paused_movie.playback = homedeck::KodiPlaybackState::kPaused;
    paused_movie.title = "A Movie";
    EXPECT_EQ(homedeck::KodiWidgetLine(Connected(paused_movie)), "Paused: A Movie");

    // Playing but no title resolved yet (add-on playback before the first
    // notification) - no placeholder name.
    homedeck::KodiNowPlaying playing_unknown;
    playing_unknown.playback = homedeck::KodiPlaybackState::kPlaying;
    EXPECT_EQ(homedeck::KodiWidgetLine(Connected(playing_unknown)), "Playing");
}

TEST(KodiNowPlayingSubtitleTest, InactiveSaysNothingPlaying) {
    EXPECT_EQ(homedeck::KodiNowPlayingSubtitle({}), "Nothing playing");
}

TEST(KodiNowPlayingSubtitleTest, EpisodeShowsShowSeasonEpisodeAndTitle) {
    EXPECT_EQ(homedeck::KodiNowPlayingSubtitle(Episode()), "The Show   S3E7  -  The One With The Test");
}

TEST(KodiNowPlayingSubtitleTest, EpisodeWithoutSeasonEpisodeOmitsTheCode) {
    homedeck::KodiNowPlaying np = Episode();
    np.season = -1;
    np.episode = -1;
    EXPECT_EQ(homedeck::KodiNowPlayingSubtitle(np), "The Show  -  The One With The Test");
}

TEST(KodiNowPlayingSubtitleTest, MovieShowsJustTheTitle) {
    homedeck::KodiNowPlaying np;
    np.playback = homedeck::KodiPlaybackState::kPlaying;
    np.title = "A Movie";
    np.media_type = "movie";
    EXPECT_EQ(homedeck::KodiNowPlayingSubtitle(np), "A Movie");
}

TEST(FormatKodiClockTest, MinutesAndSeconds) {
    EXPECT_EQ(homedeck::FormatKodiClock(0), "0:00");
    EXPECT_EQ(homedeck::FormatKodiClock(9000), "0:09");
    EXPECT_EQ(homedeck::FormatKodiClock(63000), "1:03");
    EXPECT_EQ(homedeck::FormatKodiClock(20 * 60 * 1000), "20:00");
}

TEST(FormatKodiClockTest, IncludesHoursOnlyWhenNonZero) {
    EXPECT_EQ(homedeck::FormatKodiClock(3723000), "1:02:03");
    EXPECT_EQ(homedeck::FormatKodiClock(3600000), "1:00:00");
}

TEST(FormatKodiClockTest, NegativeClampsToZero) {
    EXPECT_EQ(homedeck::FormatKodiClock(-5000), "0:00");
}
