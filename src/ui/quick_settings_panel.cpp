#include "ui/quick_settings_panel.h"

#include "ui/status_bar.h"
#include "ui/theme.h"

#include <iostream>
#include <string>

namespace homedeck {

namespace {

constexpr int32_t kPanelWidth = 340;
// Left/right padding wider than top/bottom - lv_slider's knob is drawn
// centered on the track's own end points, so it extends past the
// track's nominal bounding box by roughly its own radius. Equal padding
// on every side would leave the knob visually crowding the panel edge
// at 0%/100%.
constexpr int32_t kPanelHorizontalPad = 24;
constexpr int32_t kPanelVerticalPad = 16;
// Between a control's own label and its slider - tight, so the pairing
// reads as one unit.
constexpr int32_t kLabelToSliderGap = 6;
// Between the brightness control and the volume control - wider than
// kLabelToSliderGap so the two controls read as visually distinct groups
// rather than one undifferentiated stack.
constexpr int32_t kBetweenControlsGap = 24;
constexpr const char* kPowerModuleId = "power";
constexpr const char* kBrightnessKey = "brightness";
constexpr const char* kAudioModuleId = "audio";
constexpr const char* kVolumeKey = "volume";

void OnScrimClicked(lv_event_t* e) {
    auto* panel = static_cast<QuickSettingsPanel*>(lv_event_get_user_data(e));
    panel->Hide();
}

void OnBrightnessChanged(lv_event_t* e) {
    auto* power_manager = static_cast<PowerManager*>(lv_event_get_user_data(e));
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    power_manager->SetActiveBrightnessPercent(lv_slider_get_value(slider));
}

void OnBrightnessReleased(lv_event_t* e) {
    auto* storage = static_cast<Storage*>(lv_event_get_user_data(e));
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    // A failed write here only risks the slider's position not surviving
    // a reboot - self-heals on the next successful release - but
    // silently, with no trace, unless reported now.
    if (!storage->SetSetting(kPowerModuleId, kBrightnessKey, 1, std::to_string(lv_slider_get_value(slider)))) {
        std::cerr << "QuickSettingsPanel: failed to persist brightness setting\n";
    }
}

void OnVolumeChanged(lv_event_t* e) {
    auto* notification_sound = static_cast<NotificationSound*>(lv_event_get_user_data(e));
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    notification_sound->SetVolumePercent(lv_slider_get_value(slider));
}

void OnVolumeReleased(lv_event_t* e) {
    auto* storage = static_cast<Storage*>(lv_event_get_user_data(e));
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    // See OnBrightnessReleased's comment above - same reasoning.
    if (!storage->SetSetting(kAudioModuleId, kVolumeKey, 1, std::to_string(lv_slider_get_value(slider)))) {
        std::cerr << "QuickSettingsPanel: failed to persist volume setting\n";
    }
}

// A text caption above the slider, used for both controls - consistent
// styling, not a per-row choice. Icon-flanked sliders (mute/max glyphs
// either side, no caption) aren't used instead: LVGL's bundled symbol
// font has a matching pair for volume (LV_SYMBOL_MUTE/LV_SYMBOL_
// VOLUME_MAX) but no "brightness" glyph at all (LV_SYMBOL_TINT/
// LV_SYMBOL_IMAGE are a paint drop and a picture frame, not brightness),
// so icons for one row without the other would read as visually
// inconsistent. A unified icon pass for both is tracked as an M7 polish
// item instead (see docs/roadmap.md).
// Label + slider as direct children of parent (panel_) - not wrapped in
// a sub-container. A LV_SIZE_CONTENT wrapper sized to exactly fit its
// children clips at that exact box by default, but lv_slider's knob is
// drawn wider than the slider's own logical bounds (LVGL's default
// theme, lv_theme_default.c's knob style) - a wrapper here clipped it at
// the top/bottom always and at the left/right at the 0%/100% extremes.
// Flat children avoid that clipping boundary entirely and rely on
// panel_'s own generous padding (kPanelHorizontalPad/kPanelVerticalPad)
// to contain the knob instead.
lv_obj_t* CreateLabeledSlider(lv_obj_t* parent, const char* label_text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, kBodyFont, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, label_text);

    lv_obj_t* slider = lv_slider_create(parent);
    lv_obj_set_width(slider, LV_PCT(100));
    return slider;
}

}  // namespace

QuickSettingsPanel::QuickSettingsPanel(EventBus& event_bus, PowerManager& power_manager,
                                        NotificationSound& notification_sound, Storage& storage)
    : power_manager_(power_manager), notification_sound_(notification_sound), storage_(storage) {
    scrim_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scrim_, LV_PCT(100), LV_PCT(100));
    lv_obj_align(scrim_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(scrim_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scrim_, 0, 0);
    lv_obj_clear_flag(scrim_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scrim_, OnScrimClicked, LV_EVENT_CLICKED, this);

    panel_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel_, kPanelWidth, LV_SIZE_CONTENT);
    lv_obj_align(panel_, LV_ALIGN_TOP_RIGHT, -12, StatusBar::kHeight);
    lv_obj_set_style_bg_color(panel_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(panel_, kPanelVerticalPad, 0);
    lv_obj_set_style_pad_hor(panel_, kPanelHorizontalPad, 0);
    // The base gap between every direct child (label-to-its-own-slider,
    // slider-to-next-label alike) - kBetweenControlsGap is layered on top
    // of this via brightness_slider_'s own bottom margin below, since
    // flex's pad_row can't apply a different gap between one specific
    // pair of children.
    lv_obj_set_style_pad_row(panel_, kLabelToSliderGap, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);

    brightness_slider_ = CreateLabeledSlider(panel_, "Brightness");
    lv_slider_set_range(brightness_slider_, PowerManager::kMinActiveBrightnessPercent, 100);
    lv_obj_add_event_cb(brightness_slider_, OnBrightnessChanged, LV_EVENT_VALUE_CHANGED, &power_manager_);
    lv_obj_add_event_cb(brightness_slider_, OnBrightnessReleased, LV_EVENT_RELEASED, &storage_);
    // On top of panel_'s own kLabelToSliderGap pad_row, widening the gap
    // specifically after this slider (not a property of the row spacing
    // itself, which is uniform) so the two controls read as visually
    // distinct groups rather than one undifferentiated stack.
    lv_obj_set_style_margin_bottom(brightness_slider_, kBetweenControlsGap - kLabelToSliderGap, 0);

    volume_slider_ = CreateLabeledSlider(panel_, "Device volume");
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_obj_add_event_cb(volume_slider_, OnVolumeChanged, LV_EVENT_VALUE_CHANGED, &notification_sound_);
    lv_obj_add_event_cb(volume_slider_, OnVolumeReleased, LV_EVENT_RELEASED, &storage_);

    subscription_ = event_bus.SubscribeUi<QuickSettingsRequestedEvent>(
        [this](const QuickSettingsRequestedEvent&) { Show(); });
}

QuickSettingsPanel::~QuickSettingsPanel() {
    lv_obj_del(panel_);
    lv_obj_del(scrim_);
}

void QuickSettingsPanel::Show() {
    lv_slider_set_value(brightness_slider_, power_manager_.ActiveBrightnessPercent(), LV_ANIM_OFF);
    lv_slider_set_value(volume_slider_, notification_sound_.VolumePercent(), LV_ANIM_OFF);
    lv_obj_clear_flag(scrim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void QuickSettingsPanel::Hide() {
    lv_obj_add_flag(scrim_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace homedeck
