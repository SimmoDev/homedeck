#include "core/kodi_json.h"

#include "core/json_request.h"

namespace homedeck::kodi_json {

// See harmony_connection.cpp's ParseBoundedJson() - every frame here
// also comes off an unauthenticated LAN transport (ADR-0030).
nlohmann::json ParseBoundedJson(const std::string& text) {
    if (ExceedsJsonNestingDepth(text)) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

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

std::vector<KodiMovie> ParseMovies(const std::string& text) {
    std::vector<KodiMovie> movies;
    // Named, not inline in the ResultArray() call - ResultArray() returns
    // a pointer into whatever nlohmann::json it's given, so the parsed
    // value has to outlive that pointer's use below, not end at the end
    // of the call expression the way an inline temporary would.
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "movies");
    if (array == nullptr) {
        return movies;
    }
    movies.reserve(array->size());
    for (const auto& m : *array) {
        KodiMovie movie;
        movie.movieid = GetInt(m, "movieid", -1);
        movie.title = TitleOrLabel(m);
        movie.year = static_cast<int>(GetInt(m, "year", 0));
        movie.resume_position_ms = ResumePositionMs(m);
        movies.push_back(std::move(movie));
    }
    return movies;
}

std::vector<KodiTvShow> ParseTvShows(const std::string& text) {
    std::vector<KodiTvShow> shows;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "tvshows");
    if (array == nullptr) {
        return shows;
    }
    shows.reserve(array->size());
    for (const auto& s : *array) {
        KodiTvShow show;
        show.tvshowid = GetInt(s, "tvshowid", -1);
        show.title = TitleOrLabel(s);
        show.year = static_cast<int>(GetInt(s, "year", 0));
        show.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        show.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
        shows.push_back(std::move(show));
    }
    return shows;
}

std::vector<KodiSeason> ParseSeasons(const std::string& text) {
    std::vector<KodiSeason> seasons;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "seasons");
    if (array == nullptr) {
        return seasons;
    }
    seasons.reserve(array->size());
    for (const auto& s : *array) {
        KodiSeason season;
        season.season = static_cast<int>(GetInt(s, "season", 0));
        season.label = TitleOrLabel(s);
        season.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        season.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
        seasons.push_back(std::move(season));
    }
    return seasons;
}

std::vector<KodiEpisode> ParseEpisodes(const std::string& text) {
    std::vector<KodiEpisode> episodes;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "episodes");
    if (array == nullptr) {
        return episodes;
    }
    episodes.reserve(array->size());
    for (const auto& e : *array) {
        KodiEpisode episode;
        episode.episodeid = GetInt(e, "episodeid", -1);
        episode.episode = static_cast<int>(GetInt(e, "episode", 0));
        episode.title = TitleOrLabel(e);
        episode.resume_position_ms = ResumePositionMs(e);
        episodes.push_back(std::move(episode));
    }
    return episodes;
}

std::vector<KodiArtist> ParseArtists(const std::string& text) {
    std::vector<KodiArtist> artists;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "artists");
    if (array == nullptr) {
        return artists;
    }
    artists.reserve(array->size());
    for (const auto& a : *array) {
        KodiArtist artist;
        artist.artistid = GetInt(a, "artistid", -1);
        // "artist" (Kodi's own artist-name field), falling back to the
        // always-present `label` - same fallback shape as TitleOrLabel(),
        // just a different primary field name (artists have no "title").
        std::string name = GetString(a, "artist", "");
        artist.name = !name.empty() ? name : GetString(a, "label", "");
        artists.push_back(std::move(artist));
    }
    return artists;
}

std::vector<KodiAlbum> ParseAlbums(const std::string& text) {
    std::vector<KodiAlbum> albums;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "albums");
    if (array == nullptr) {
        return albums;
    }
    albums.reserve(array->size());
    for (const auto& a : *array) {
        KodiAlbum album;
        album.albumid = GetInt(a, "albumid", -1);
        album.title = TitleOrLabel(a);
        album.year = static_cast<int>(GetInt(a, "year", 0));
        albums.push_back(std::move(album));
    }
    return albums;
}

std::vector<KodiSong> ParseSongs(const std::string& text) {
    std::vector<KodiSong> songs;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "songs");
    if (array == nullptr) {
        return songs;
    }
    songs.reserve(array->size());
    for (const auto& s : *array) {
        KodiSong song;
        song.songid = GetInt(s, "songid", -1);
        song.track = static_cast<int>(GetInt(s, "track", 0));
        song.title = TitleOrLabel(s);
        song.duration_seconds = static_cast<int>(GetInt(s, "duration", 0));
        songs.push_back(std::move(song));
    }
    return songs;
}

// Shared by Files.GetSources' reply ("sources") and Files.GetDirectory's
// ("files") - result_key and all_folders are the only differences
// between the two shapes. A source item carries no "filetype" field at
// all - it is always a folder by definition (a configured root), so
// all_folders=true skips the "filetype" check rather than reading a field
// that isn't there and misreading every source as a file.
std::vector<KodiFileItem> ParseFileItems(const std::string& text, const char* result_key, bool all_folders) {
    std::vector<KodiFileItem> items;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, result_key);
    if (array == nullptr) {
        return items;
    }
    items.reserve(array->size());
    for (const auto& f : *array) {
        KodiFileItem item;
        item.path = GetString(f, "file", "");
        item.label = TitleOrLabel(f);
        item.is_folder = all_folders || GetString(f, "filetype", "") == "directory";
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<KodiChannelGroup> ParseChannelGroups(const std::string& text) {
    std::vector<KodiChannelGroup> groups;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "channelgroups");
    if (array == nullptr) {
        return groups;
    }
    groups.reserve(array->size());
    for (const auto& g : *array) {
        KodiChannelGroup group;
        group.channelgroupid = GetInt(g, "channelgroupid", -1);
        group.label = TitleOrLabel(g);
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<KodiChannel> ParseChannels(const std::string& text) {
    std::vector<KodiChannel> channels;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "channels");
    if (array == nullptr) {
        return channels;
    }
    channels.reserve(array->size());
    for (const auto& c : *array) {
        KodiChannel channel;
        channel.channelid = GetInt(c, "channelid", -1);
        channel.label = TitleOrLabel(c);
        channels.push_back(std::move(channel));
    }
    return channels;
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
