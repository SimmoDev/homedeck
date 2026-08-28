#include "ui/kodi_display.h"

namespace homedeck {

namespace {

std::string PrimaryTitle(const KodiNowPlaying& np) {
    if (!np.show_title.empty()) {
        return np.show_title;
    }
    if (!np.title.empty()) {
        return np.title;
    }
    return "Something";
}

}  // namespace

std::string KodiWidgetLine(const KodiSnapshot& snapshot) {
    switch (snapshot.state) {
        case KodiConnectionState::kConnecting:
            return "Connecting...";
        case KodiConnectionState::kError:
            return "Not reachable";
        case KodiConnectionState::kDisconnected:
            // More than one instance answered discovery but none is
            // saved - the user has to pick (ADR-0030). Otherwise there's
            // simply nothing configured to connect to.
            return snapshot.discovered.size() > 1 ? "Choose a Kodi in settings" : "Not configured";
        case KodiConnectionState::kConnected:
            break;
    }
    switch (snapshot.now_playing.playback) {
        case KodiPlaybackState::kInactive:
            return "Idle";
        case KodiPlaybackState::kPaused:
            return "Paused: " + PrimaryTitle(snapshot.now_playing);
        case KodiPlaybackState::kPlaying:
            return "Playing: " + PrimaryTitle(snapshot.now_playing);
    }
    return "Idle";
}

std::string KodiNowPlayingSubtitle(const KodiNowPlaying& np) {
    if (np.playback == KodiPlaybackState::kInactive) {
        return "Nothing playing";
    }
    if (!np.show_title.empty()) {
        std::string line = np.show_title;
        if (np.season >= 0 && np.episode >= 0) {
            line += "   S" + std::to_string(np.season) + "E" + std::to_string(np.episode);
        }
        if (!np.title.empty() && np.title != np.show_title) {
            line += "  -  " + np.title;
        }
        return line;
    }
    return !np.title.empty() ? np.title : "Playing";
}

std::string FormatKodiClock(long long milliseconds) {
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    long long total_seconds = milliseconds / 1000;
    long long hours = total_seconds / 3600;
    long long minutes = (total_seconds % 3600) / 60;
    long long seconds = total_seconds % 60;

    std::string out;
    if (hours > 0) {
        out += std::to_string(hours) + ":";
        if (minutes < 10) {
            out += "0";
        }
    }
    out += std::to_string(minutes) + ":";
    if (seconds < 10) {
        out += "0";
    }
    out += std::to_string(seconds);
    return out;
}

}  // namespace homedeck
