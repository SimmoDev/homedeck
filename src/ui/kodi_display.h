#pragma once

#include "core/kodi_client.h"

#include <string>

namespace homedeck {

// Pure display-string helpers for the Kodi Touch UI - LVGL-free and
// built into homedeck_core so tests cover them directly, the same
// reasoning ui/text_format.h / ui/activity_start_tracker.h already
// document.

// The dashboard KodiWidget's single status line.
std::string KodiWidgetLine(const KodiSnapshot& snapshot);

// NowPlayingScreen's subtitle: "Show Name   S3E7" for an episode, the
// title for a movie/song, "Nothing playing" when inactive.
std::string KodiNowPlayingSubtitle(const KodiNowPlaying& now_playing);

// "5:03" / "1:05:03" from a millisecond count - hours shown only when
// non-zero.
std::string FormatKodiClock(long long milliseconds);

}  // namespace homedeck
