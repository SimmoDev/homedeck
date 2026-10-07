#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/navigation.h"
#include "ui/screen_loader.h"
#include "ui/status_bar.h"
#include "ui/virtual_list.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace homedeck {

// The Kodi module's Files library-browse screen (docs/roadmap.md's M4b
// "Media browsing" item, files slice) - reached from NowPlayingScreen's
// "Files" button. Mirrors Kodi's own Videos > Files menu: the
// configured video-source list (Files.GetSources("video")), then
// Files.GetDirectory(path, "video") at whatever depth the underlying
// filesystem has (a show's season folders, a season's episode files,
// ...) - the fallback view for content not (yet) scraped into the video
// library that KodiMoviesScreen/KodiTvShowsScreen browse instead.
//
// Unlike every other browse screen (a fixed number of levels, each its
// own sibling container), folder depth here is unbounded, so this
// screen keeps a `path_stack_` of {path, label} entered so far and
// rebuilds one shared list container in place rather than allocating a
// container per level. "Back" pops one entry (or returns to the source
// list once empty) - same "one level up, never straight to the top or
// Home" rule the fixed-depth screens follow.
//
// No Play/Resume choice, same reasoning as KodiMusicScreen: this is the
// raw-filesystem fallback, and a tapped file plays directly via
// KodiClient::PlayFile() (Player.Open with a raw {"file": path} item,
// not a library id).
class KodiFilesScreen {
public:
    KodiFilesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                    KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~KodiFilesScreen();

    KodiFilesScreen(const KodiFilesScreen&) = delete;
    KodiFilesScreen& operator=(const KodiFilesScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();  // hint_label_ vs content_, per KodiClient::Snapshot()
    // Rebinds list_ to a fetch reply - path_stack_ already reflects
    // the level this reply is for (Enter()/GoBack() push/pop before
    // requesting), so this only needs the items themselves.
    void RebuildList(const std::vector<KodiFileItem>& items, bool truncated);
    // Requests the source list (path_stack_ empty) or the directory at
    // the top of path_stack_ - whichever the stack's current state
    // means, called after every push/pop.
    void RequestCurrentLevel();
    void Enter(const KodiFileItem& item);  // folder: push and descend; file: play and go to Now Playing
    void GoBack();

    static void OnBackButtonClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    ScreenLoader loader_;  // requests the top-level list when first shown, and after a reconnect
    lv_obj_t* hint_label_;
    lv_obj_t* content_;      // == ScreenChrome's content_container
    lv_obj_t* back_button_;  // hidden at the source-list level (path_stack_ empty)
    lv_obj_t* heading_label_;

    // Declared after root_ so it is destroyed before it is: see
    // VirtualList's own lifetime note.
    std::unique_ptr<VirtualList> list_;

    // {path, label} for each folder entered so far, outermost first -
    // GoBack() pops one; RequestCurrentLevel() reads path_stack_.back()
    // (or requests sources if empty).
    std::vector<std::pair<std::string, std::string>> path_stack_;

    // The current level's own last-fetched items, and the path that
    // fetch was for - filters a reply for a level the user has since
    // navigated away from (a slow query racing a fast back/enter tap),
    // same reasoning as the fixed-depth screens' tvshowid/season checks.
    std::string requested_path_;
    std::vector<KodiFileItem> items_;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription files_sub_;
};

}  // namespace homedeck
