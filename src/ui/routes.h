#pragma once

namespace homedeck::routes {

// The route ids screens are registered under in AppCore and navigated to
// by name. Navigation::GoTo() ignores an unregistered id, so every caller
// and registration shares these constants instead of repeating the string.
inline constexpr char kDashboard[] = "dashboard";
inline constexpr char kWifiSetup[] = "wifi-setup";
inline constexpr char kHarmonyActivities[] = "harmony-activities";
inline constexpr char kHarmonyDevices[] = "harmony-devices";
inline constexpr char kKodiNowPlaying[] = "kodi-now-playing";
inline constexpr char kKodiRemote[] = "kodi-remote";
inline constexpr char kKodiMovies[] = "kodi-movies";
inline constexpr char kKodiTvShows[] = "kodi-tv-shows";
inline constexpr char kKodiMusic[] = "kodi-music";
inline constexpr char kKodiFiles[] = "kodi-files";
inline constexpr char kKodiLiveTv[] = "kodi-live-tv";

}  // namespace homedeck::routes
