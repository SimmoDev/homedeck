#pragma once

#include "core/kodi_client.h"
#include "third_party/nlohmann/json.hpp"

#include <string>
#include <vector>

namespace homedeck::kodi_json {

// Turns Kodi's JSON-RPC replies into the structs KodiClient publishes.
// Kodi's 9090 API has no authentication (ADR-0030), so every function here
// treats its input as untrusted: nothing throws or aborts on a missing or
// mistyped field, which is `std::abort()` on firmware (no C++ exceptions).
// Portable and host-tested through KodiClient's listing and notification
// tests.

// Typed field readers: the receiver and the field are type-checked before
// extraction, and `fallback` is returned for anything else.
long long GetInt(const nlohmann::json& j, const char* key, long long fallback);
double GetDouble(const nlohmann::json& j, const char* key, double fallback);
// GetInt() limited to [min, max]: a value outside the range is clamped, so
// a hostile reply cannot push an out-of-range number into later arithmetic
// or an int narrowing.
int GetBoundedInt(const nlohmann::json& j, const char* key, int fallback, int min, int max);
std::string GetString(const nlohmann::json& j, const char* key, const std::string& fallback);
bool GetBool(const nlohmann::json& j, const char* key, bool fallback);

// Ranges Kodi's volume (percent) and playback speed (multiples of normal)
// are clamped to wherever they are read from a reply.
constexpr int kMaxVolume = 100;
constexpr int kMaxSpeed = 1000;
inline int ClampVolume(long long volume) { return static_cast<int>(volume < 0 ? 0 : (volume > kMaxVolume ? kMaxVolume : volume)); }
inline int ClampSpeed(long long speed) {
    return static_cast<int>(speed < -kMaxSpeed ? -kMaxSpeed : (speed > kMaxSpeed ? kMaxSpeed : speed));
}

// Kodi's {hours,minutes,seconds,milliseconds} time object, in milliseconds.
// Each field is clamped to [0, 1,000,000] so the arithmetic cannot overflow.
long long MillisFromTimeObject(const nlohmann::json& t);
KodiPlaybackState PlaybackFromSpeed(int speed);

// The named array inside a reply's "result" object, or null for anything
// short of a well-formed object containing it.
const nlohmann::json* ResultArray(const nlohmann::json& parsed, const char* key);

// Fills `now_playing` from a notification's or Player.GetItem's `item`
// object, leaving blank or missing fields at their current values.
void ApplyItemFields(const nlohmann::json& item, KodiNowPlaying& now_playing);

// Listing replies; a malformed reply yields an empty list.
std::vector<KodiMovie> ParseMovies(const std::string& text);
std::vector<KodiTvShow> ParseTvShows(const std::string& text);
std::vector<KodiSeason> ParseSeasons(const std::string& text);
std::vector<KodiEpisode> ParseEpisodes(const std::string& text);
std::vector<KodiArtist> ParseArtists(const std::string& text);
std::vector<KodiAlbum> ParseAlbums(const std::string& text);
std::vector<KodiSong> ParseSongs(const std::string& text);
std::vector<KodiFileItem> ParseFileItems(const std::string& text, const char* result_key, bool all_folders);
std::vector<KodiChannelGroup> ParseChannelGroups(const std::string& text);
std::vector<KodiChannel> ParseChannels(const std::string& text);

}  // namespace homedeck::kodi_json
