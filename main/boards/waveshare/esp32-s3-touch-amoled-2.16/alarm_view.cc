#include "alarm_view.h"

#include <cstring>
#include <ctime>

#include <material_symbols.h>

#include "display/lvgl_display/lvgl_theme.h"

namespace {

constexpr int kBaselineNudge = -2;

// About the I2S DMA tail left over when playback is dropped.
constexpr int kFadeMs = 220;

}  // namespace

AlarmView::AlarmView(Alarm& alarm) : alarm_(alarm) {}

lv_obj_t* AlarmView::MakeRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void AlarmView::Build(lv_obj_t* parent, LvglTheme* theme) {
    if (lv_obj_is_valid(list_)) {
        return;
    }
    if (parent == nullptr || theme == nullptr) {
        return;
    }
    parent_ = parent;
    theme_ = theme;

    hint_ = lv_label_create(parent);
    lv_obj_set_width(hint_, lv_pct(100));
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint_, theme->text_color(), 0);
    lv_label_set_text(hint_, "说\"每天7点叫我\"即可添加");

    empty_ = lv_label_create(parent);
    lv_obj_set_width(empty_, lv_pct(100));
    lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty_, theme->text_color(), 0);
    lv_label_set_text(empty_, "还没有闹钟");
    lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);

    list_ = lv_obj_create(parent);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_height(list_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, 6, 0);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

    Refresh();
}

void AlarmView::Reset() {
    theme_ = nullptr;
    parent_ = nullptr;
    hint_ = nullptr;
    empty_ = nullptr;
    list_ = nullptr;
}

void AlarmView::Refresh() {
    if (!lv_obj_is_valid(list_)) {
        return;
    }
    lv_obj_clean(list_);

    auto* theme = theme_;
    if (theme == nullptr) {
        return;
    }

    for (int i = 0; i < alarm_.Count(); i++) {
        const Alarm::Entry entry = alarm_.Get(i);

        lv_obj_t* row = MakeRow(list_);
        lv_obj_t* time_label = lv_label_create(row);
        lv_label_set_text_fmt(time_label, "%02d:%02d", entry.hour, entry.minute);
        lv_obj_set_style_text_color(time_label, theme->text_color(), 0);
        lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* days_label = lv_label_create(row);
        lv_label_set_text(days_label, WeekdayText(entry.weekdays));
        lv_obj_set_style_text_color(days_label, theme->text_color(), 0);
        lv_obj_align(days_label, LV_ALIGN_LEFT_MID, 96, 0);

        lv_obj_t* toggle = lv_switch_create(row);
        lv_obj_set_size(toggle, 46, 23);
        lv_obj_set_ext_click_area(toggle, 12);
        lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -54, kBaselineNudge);
        if (entry.enabled) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_set_user_data(toggle, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(toggle, OnToggle, LV_EVENT_VALUE_CHANGED, this);

        // LV_SYMBOL_* needs LVGL's symbol font, which this theme does not ship.
        lv_obj_t* remove = lv_label_create(row);
        lv_obj_set_style_text_font(remove, theme->large_icon_font()->font(), 0);
        lv_label_set_text(remove, MATERIAL_SYMBOLS_DELETE);
        lv_obj_set_style_text_color(remove, lv_color_hex(0xC06060), 0);
        lv_obj_set_ext_click_area(remove, 12);
        lv_obj_add_flag(remove, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(remove, LV_ALIGN_RIGHT_MID, -4, kBaselineNudge);
        lv_obj_set_user_data(remove, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(remove, OnRemove, LV_EVENT_CLICKED, this);
    }

    if (lv_obj_is_valid(empty_)) {
        if (alarm_.Count() == 0) {
            lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AlarmView::OnToggle(lv_event_t* event) {
    auto* self = static_cast<AlarmView*>(lv_event_get_user_data(event));
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (self == nullptr || toggle == nullptr) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(toggle)));
    self->alarm_.SetEnabled(index, lv_obj_has_state(toggle, LV_STATE_CHECKED));
}

void AlarmView::OnRemove(lv_event_t* event) {
    auto* self = static_cast<AlarmView*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (self == nullptr || target == nullptr) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    self->alarm_.Remove(index);
}

const char* AlarmView::WeekdayText(uint8_t mask) {
    if (mask == 0) {
        return "一次";
    }
    if (mask == Alarm::kEveryDay) {
        return "每天";
    }
    if (mask == 0x1F) {
        return "工作日";
    }
    // Three UTF-8 bytes per day name, so seven days need 22.
    static char buffer[24];
    static const char* kNames[] = {"一", "二", "三", "四", "五", "六", "日"};
    buffer[0] = '\0';
    for (int day = 0; day < 7; day++) {
        if (mask & (1u << day)) {
            strncat(buffer, kNames[day], sizeof(buffer) - strlen(buffer) - 1);
        }
    }
    return buffer;
}

void AlarmView::ShowRinging(LvglTheme* theme) {
    if (lv_obj_is_valid(ringing_page_) || theme == nullptr) {
        return;
    }

    // Above every screen and survives screen changes, so nothing can cover it.
    ringing_page_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ringing_page_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(ringing_page_, 0, 0);
    lv_obj_set_style_radius(ringing_page_, 0, 0);
    lv_obj_set_style_border_width(ringing_page_, 0, 0);
    lv_obj_set_style_pad_all(ringing_page_, 0, 0);
    lv_obj_set_style_bg_color(ringing_page_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(ringing_page_, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(ringing_page_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ringing_page_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        ringing_page_,
        [](lv_event_t* event) {
            auto* self = static_cast<AlarmView*>(lv_event_get_user_data(event));
            if (self != nullptr && self->stop_handler_) {
                self->stop_handler_();
            }
        },
        LV_EVENT_CLICKED, this);

    ringing_clock_ = lv_label_create(ringing_page_);
    lv_obj_set_style_text_font(ringing_clock_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(ringing_clock_, theme->text_color(), 0);
    lv_label_set_text(ringing_clock_, "--:--");
    lv_obj_align(ringing_clock_, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t* hint = lv_label_create(ringing_page_);
    lv_obj_set_style_text_color(hint, theme->text_color(), 0);
    lv_label_set_text(hint, "点击停止闹钟");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    // The animation dies with the label.
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ringing_clock_);
    lv_anim_set_values(&anim, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 550);
    lv_anim_set_playback_duration(&anim, 550);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
    });
    lv_anim_start(&anim);

    RefreshClock();
}

void AlarmView::RefreshClock() {
    if (!lv_obj_is_valid(ringing_clock_)) {
        return;
    }
    auto now = time(nullptr);
    tm local_tm = {};
    if (localtime_r(&now, &local_tm) == nullptr || local_tm.tm_year + 1900 < 2024) {
        lv_label_set_text(ringing_clock_, "--:--");
        return;
    }
    char text[16];
    strftime(text, sizeof(text), "%H:%M", &local_tm);
    lv_label_set_text(ringing_clock_, text);
}

void AlarmView::HideRinging() {
    if (!lv_obj_is_valid(ringing_page_)) {
        return;
    }
    lv_obj_t* page = ringing_page_;
    // The fade below owns the page from here on.
    ringing_page_ = nullptr;
    ringing_clock_ = nullptr;
    lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, page);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, kFadeMs);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
    });
    lv_anim_start(&anim);

    // Deleting from inside the animation's own callback is easy to get wrong.
    lv_timer_create(
        [](lv_timer_t* timer) {
            auto* fading = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
            lv_timer_delete(timer);
            if (lv_obj_is_valid(fading)) {
                lv_obj_delete(fading);
            }
        },
        kFadeMs + 20, page);
}
