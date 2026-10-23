#pragma once

#include <functional>

#include <lvgl.h>

#include "alarm.h"

class LvglTheme;

// Owns the alarm widgets only. The page container, its gesture handlers and its
// visibility stay with the display subclass, which is the only place allowed to
// touch LcdDisplay's protected widgets.
class AlarmView {
public:
    explicit AlarmView(Alarm& alarm);

    // Safe to call twice; the second call is ignored.
    void Build(lv_obj_t* parent, LvglTheme* theme);
    void Reset();
    bool built() const { return lv_obj_is_valid(list_) != 0; }

    // Caller holds the display lock.
    void Refresh();

    // Lives on lv_layer_top(), so nothing drawn later can cover it.
    void ShowRinging(LvglTheme* theme);
    void HideRinging();
    void RefreshClock();
    bool ringing_page_alive() const { return lv_obj_is_valid(ringing_page_) != 0; }

    void SetStopHandler(std::function<void()> handler) { stop_handler_ = std::move(handler); }

private:
    static void OnToggle(lv_event_t* event);
    static void OnRemove(lv_event_t* event);
    static const char* WeekdayText(uint8_t mask);
    static lv_obj_t* MakeRow(lv_obj_t* parent);

    Alarm& alarm_;
    LvglTheme* theme_ = nullptr;
    lv_obj_t* parent_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* empty_ = nullptr;
    lv_obj_t* list_ = nullptr;

    lv_obj_t* ringing_page_ = nullptr;
    lv_obj_t* ringing_clock_ = nullptr;

    std::function<void()> stop_handler_;
};
