#include "ui/clock_widget.h"

#include "core/clock.h"
#include "ui/theme.h"
#include "ui/time_format.h"
#include "ui/widget_tile.h"

namespace homedeck {

ClockWidget::ClockWidget(lv_obj_t* parent, EventBus& event_bus) {
    // Stacks time above date, both centered in the cell - see
    // CreateWidgetTileRoot() for the shared tile layout every widget
    // builds on.
    root_ = CreateWidgetTileRoot(parent);

    time_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
    // Blank until the first ClockTickEvent, not a fabricated time - same
    // reasoning as status_bar.cpp's clock_label_.
    lv_label_set_text(time_label_, "");

    date_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(date_label_, kBodyFont, 0);
    lv_label_set_text(date_label_, "");

    clock_subscription_ = event_bus.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent& event) {
        char time_text[16];
        char date_text[32];
        FormatLocalTime(event.time, "%H:%M", time_text, sizeof(time_text));
        FormatLocalTime(event.time, "%a %d %b", date_text, sizeof(date_text));
        lv_label_set_text(time_label_, time_text);
        lv_label_set_text(date_label_, date_text);
    });
}

}  // namespace homedeck
