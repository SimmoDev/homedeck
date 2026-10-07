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

// Parses a frame, returning a discarded value for malformed or
// deeply nested text.
nlohmann::json ParseBoundedJson(const std::string& text);

// Typed field readers: the receiver and the field are type-checked before
// extraction, and `fallback` is returned for anything else.
long long GetInt(const nlohmann::json& j, const char* key, long long fallback);
double GetDouble(const nlohmann::json& j, const char* key, double fallback);
std::string GetString(const nlohmann::json& j, const char* key, const std::string& fallback);
bool GetBool(const nlohmann::json& j, const char* key, bool fallback);

// Kodi's {hours,minutes,seconds,milliseconds} time object, in milliseconds.
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
