#include "ui/clock_widget.h"

#include "core/clock.h"

#include <ctime>

namespace homedeck {

namespace {

// localtime_r is POSIX; matches Linux being the only currently-verified
// simulator platform (see DEVELOPMENT.md) - same precedent as
// status_bar.cpp's own time formatting.
void FormatTime(std::chrono::system_clock::time_point time, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, "%H:%M", &tm);
}

void FormatDate(std::chrono::system_clock::time_point time, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, "%a %d %b", &tm);
}

}  // namespace

ClockWidget::ClockWidget(lv_obj_t* parent, EventBus& event_bus) {
    root_ = lv_obj_create(parent);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    // Column flex, centered both axes - stacks time above date and
    // keeps both centered in the cell regardless of exact cell size,
    // rather than hand-tuned pixel offsets tied to today's row height.
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    time_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
    // Blank until the first ClockTickEvent, not a fabricated time - same
    // reasoning as status_bar.cpp's clock_label_.
    lv_label_set_text(time_label_, "");

    date_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(date_label_, &lv_font_montserrat_24, 0);
    lv_label_set_text(date_label_, "");

    clock_subscription_ = event_bus.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent& event) {
        char time_text[16];
        char date_text[32];
        FormatTime(event.time, time_text, sizeof(time_text));
        FormatDate(event.time, date_text, sizeof(date_text));
        lv_label_set_text(time_label_, time_text);
        lv_label_set_text(date_label_, date_text);
    });
}

}  // namespace homedeck
