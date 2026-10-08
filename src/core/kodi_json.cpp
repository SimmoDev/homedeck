#include "core/kodi_json.h"

#include "core/json_request.h"

namespace homedeck::kodi_json {

// nlohmann::json::value()/get<T>() throw json::type_error when a present
// field (or the receiver itself) is not the requested type - and firmware
// builds with C++ exceptions disabled (CONFIG_COMPILER_CXX_EXCEPTIONS is
// unset), so json.hpp's JSON_THROW resolves to std::abort() there instead
// of throw. Kodi's 9090 API has no authentication (ADR-0030), so any
// device on the LAN can trigger this with a malformed reply. These three
// helpers check both the receiver's and the field's type before
// extracting, the same hazard weather_routes.cpp's geocode parsing
// already guards against - never reach for j.value(key, default) on data
// from an external response.
long long GetInt(const nlohmann::json& j, const char* key, long long fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : fallback;
}

double GetDouble(const nlohmann::json& j, const char* key, double fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

std::string GetString(const nlohmann::json& j, const char* key, const std::string& fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

bool GetBool(const nlohmann::json& j, const char* key, bool fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

long long MillisFromTimeObject(const nlohmann::json& t) {
    long long seconds = GetInt(t, "hours", 0) * 3600LL + GetInt(t, "minutes", 0) * 60LL + GetInt(t, "seconds", 0);
    return seconds * 1000LL + GetInt(t, "milliseconds", 0);
}

KodiPlaybackState PlaybackFromSpeed(int speed) {
    return speed == 0 ? KodiPlaybackState::kPaused : KodiPlaybackState::kPlaying;
}

namespace {

// A movie/episode's "resume" property - {"position": <seconds, float>,
// "total": <seconds, float>} - not the {hours,minutes,seconds,
// milliseconds} shape MillisFromTimeObject() parses. 0 (no resume point)
// if absent/malformed.
long long ResumePositionMs(const nlohmann::json& item) {
    if (!item.is_object()) {
        return 0;
    }
    auto resume_it = item.find("resume");
    if (resume_it == item.end()) {
        return 0;
    }
    return static_cast<long long>(GetDouble(*resume_it, "position", 0.0) * 1000.0);
}

// title, falling back to Kodi's own always-present `label` when the
// requested `title` property comes back blank - same fallback
// ApplyItemFields() uses for Now Playing's add-on-playback case.
std::string TitleOrLabel(const nlohmann::json& item) {
    std::string title = GetString(item, "title", "");
    return !title.empty() ? title : GetString(item, "label", "");
}

}  // namespace

// Pulls the named array out of a VideoLibrary.Get*'s "result" object
// (e.g. "movies", "tvshows", "seasons", "episodes") - an empty array,
// not a parse error, for anything short of a well-formed object
// containing it (a malformed/absent field just means an empty list).
const nlohmann::json* ResultArray(const nlohmann::json& parsed, const char* key) {
    auto result_it = parsed.is_object() ? parsed.find("result") : parsed.end();
    if (result_it == parsed.end() || !result_it->is_object()) {
        return nullptr;
    }
    auto array_it = result_it->find(key);
    if (array_it == result_it->end() || !array_it->is_array()) {
        return nullptr;
    }
    return &*array_it;
}

namespace {

// Parses `text` and builds one T per element of the reply's `key` array with
// `fill(element, T&)`. `parsed` stays alive for the loop because ResultArray()
// returns a pointer into it.
template <typename T, typename Fill>
std::vector<T> ParseList(const std::string& text, const char* key, Fill fill) {
    std::vector<T> out;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, key);
    if (array == nullptr) {
        return out;
    }
    out.reserve(array->size());
    for (const auto& element : *array) {
        T item;
        fill(element, item);
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace

std::vector<KodiMovie> ParseMovies(const std::string& text) {
    return ParseList<KodiMovie>(text, "movies", [](const nlohmann::json& m, KodiMovie& movie) {
        movie.movieid = GetInt(m, "movieid", -1);
        movie.title = TitleOrLabel(m);
        movie.year = static_cast<int>(GetInt(m, "year", 0));
        movie.resume_position_ms = ResumePositionMs(m);
    });
}

std::vector<KodiTvShow> ParseTvShows(const std::string& text) {
    return ParseList<KodiTvShow>(text, "tvshows", [](const nlohmann::json& s, KodiTvShow& show) {
        show.tvshowid = GetInt(s, "tvshowid", -1);
        show.title = TitleOrLabel(s);
        show.year = static_cast<int>(GetInt(s, "year", 0));
        show.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        show.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
    });
}

std::vector<KodiSeason> ParseSeasons(const std::string& text) {
    return ParseList<KodiSeason>(text, "seasons", [](const nlohmann::json& s, KodiSeason& season) {
        season.season = static_cast<int>(GetInt(s, "season", 0));
        season.label = TitleOrLabel(s);
        season.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        season.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
    });
}

std::vector<KodiEpisode> ParseEpisodes(const std::string& text) {
    return ParseList<KodiEpisode>(text, "episodes", [](const nlohmann::json& e, KodiEpisode& episode) {
        episode.episodeid = GetInt(e, "episodeid", -1);
        episode.episode = static_cast<int>(GetInt(e, "episode", 0));
        episode.title = TitleOrLabel(e);
        episode.resume_position_ms = ResumePositionMs(e);
    });
}

std::vector<KodiArtist> ParseArtists(const std::string& text) {
    return ParseList<KodiArtist>(text, "artists", [](const nlohmann::json& a, KodiArtist& artist) {
        artist.artistid = GetInt(a, "artistid", -1);
        // "artist" (Kodi's own artist-name field), falling back to the
        // always-present `label` - same fallback shape as TitleOrLabel(),
        // just a different primary field name (artists have no "title").
        std::string name = GetString(a, "artist", "");
        artist.name = !name.empty() ? name : GetString(a, "label", "");
    });
}

std::vector<KodiAlbum> ParseAlbums(const std::string& text) {
    return ParseList<KodiAlbum>(text, "albums", [](const nlohmann::json& a, KodiAlbum& album) {
        album.albumid = GetInt(a, "albumid", -1);
        album.title = TitleOrLabel(a);
        album.year = static_cast<int>(GetInt(a, "year", 0));
    });
}

std::vector<KodiSong> ParseSongs(const std::string& text) {
    return ParseList<KodiSong>(text, "songs", [](const nlohmann::json& s, KodiSong& song) {
        song.songid = GetInt(s, "songid", -1);
        song.track = static_cast<int>(GetInt(s, "track", 0));
        song.title = TitleOrLabel(s);
        song.duration_seconds = static_cast<int>(GetInt(s, "duration", 0));
    });
}

// Shared by Files.GetSources' reply ("sources") and Files.GetDirectory's
// ("files") - result_key and all_folders are the only differences
// between the two shapes. A source item carries no "filetype" field at
// all - it is always a folder by definition (a configured root), so
// all_folders=true skips the "filetype" check rather than reading a field
// that isn't there and misreading every source as a file.
std::vector<KodiFileItem> ParseFileItems(const std::string& text, const char* result_key, bool all_folders) {
    return ParseList<KodiFileItem>(text, result_key, [all_folders](const nlohmann::json& f, KodiFileItem& item) {
        item.path = GetString(f, "file", "");
        item.label = TitleOrLabel(f);
        item.is_folder = all_folders || GetString(f, "filetype", "") == "directory";
    });
}

std::vector<KodiChannelGroup> ParseChannelGroups(const std::string& text) {
    return ParseList<KodiChannelGroup>(text, "channelgroups", [](const nlohmann::json& g, KodiChannelGroup& group) {
        group.channelgroupid = GetInt(g, "channelgroupid", -1);
        group.label = TitleOrLabel(g);
    });
}

std::vector<KodiChannel> ParseChannels(const std::string& text) {
    return ParseList<KodiChannel>(text, "channels", [](const nlohmann::json& c, KodiChannel& channel) {
        channel.channelid = GetInt(c, "channelid", -1);
        channel.label = TitleOrLabel(c);
    });
}

// Pulls title/show/season/episode/type out of a notification's or a
// Player.GetItem response's `item` object. Missing/blank fields are left
// at their struct defaults so a later, better-populated source (or the
// notification, for add-on playback - see ADR-0030) can fill them.
void ApplyItemFields(const nlohmann::json& item, KodiNowPlaying& now_playing) {
    if (!item.is_object()) {
        return;
    }
    std::string title = GetString(item, "title", "");
    if (title.empty()) {
        title = GetString(item, "label", "");  // add-on playback: `title` blank, `label` usable
    }
    if (!title.empty()) {
        now_playing.title = title;
    }
    if (item.contains("showtitle") && item["showtitle"].is_string() && !item["showtitle"].get<std::string>().empty()) {
        now_playing.show_title = item["showtitle"].get<std::string>();
    }
    if (item.contains("season") && item["season"].is_number_integer()) {
        int season = item["season"].get<int>();
        if (season >= 0) {
            now_playing.season = season;
        }
    }
    if (item.contains("episode") && item["episode"].is_number_integer()) {
        int episode = item["episode"].get<int>();
        if (episode >= 0) {
            now_playing.episode = episode;
        }
    }
    if (item.contains("type") && item["type"].is_string()) {
        std::string type = item["type"].get<std::string>();
        if (!type.empty() && type != "unknown") {
            now_playing.media_type = type;
        }
    }
}

}  // namespace homedeck::kodi_json
