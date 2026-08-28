#include "ui/kodi_widget.h"

#include "ui/kodi_display.h"
#include "ui/theme.h"
#include "ui/widget_tile.h"

namespace homedeck {

KodiWidget::KodiWidget(lv_obj_t* parent, EventBus& event_bus, KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client), navigation_(navigation) {
    root_ = CreateWidgetTileRoot(parent);

    lv_obj_t* title = lv_label_create(root_);
    lv_obj_set_style_text_font(title, kBodyFont, 0);
    lv_label_set_text(title, "Kodi");

    label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(label_, kBodyFont, 0);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent&) { Refresh(); });
    now_playing_sub_ =
        event_bus.SubscribeUi<KodiNowPlayingChangedEvent>([this](const KodiNowPlayingChangedEvent&) { Refresh(); });
}

void KodiWidget::Refresh() { lv_label_set_text(label_, KodiWidgetLine(kodi_client_.Snapshot()).c_str()); }

void KodiWidget::OnTap() { navigation_.GoTo("kodi-now-playing"); }

}  // namespace homedeck
