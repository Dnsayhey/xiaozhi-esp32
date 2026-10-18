#include "wifi_board.h"
#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <functional>
#include <string>

#include <esp_random.h>
#include <cstdlib>
#include <ctime>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch16"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out

    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    // The board installs this so a touch can take the panel out of power save.
    void SetWakeCallback(std::function<void()> callback) { wake_callback_ = std::move(callback); }

    // The board reports power save transitions. The touch that wakes the panel
    // must not also be interpreted as a tap or a swipe.
    void SetAsleep(bool asleep) { asleep_ = asleep; }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        // The status icons (network on the left, mute/battery on the right)
        // live in top_bar_ and sit flush against the screen edges, so the
        // rounded corners clip them. Inset the bar itself.
        if (top_bar_ != nullptr) {
            lv_obj_set_style_pad_left(top_bar_, DISPLAY_CORNER_INSET, 0);
            lv_obj_set_style_pad_right(top_bar_, DISPLAY_CORNER_INSET, 0);
        }
        // status_bar_ only holds centred text, but keep it clear of the
        // corners too so long strings do not run into the radius.
        lv_obj_set_style_pad_left(status_bar_, DISPLAY_CORNER_INSET, 0);
        lv_obj_set_style_pad_right(status_bar_, DISPLAY_CORNER_INSET, 0);

        // --- touch UI -------------------------------------------------------
        // The framework ships no interactive widgets, so install our own:
        // tap = start/stop talking, swipe up = the same, swipe down = settings.
        lv_obj_t* screen = lv_screen_active();
        // A swipe has to arrive as a gesture; a scrollable screen eats it.
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

        // LVGL hands input to the topmost clickable object only, and the chat
        // area covers the screen, so every layer in between needs wiring up.
        for (lv_obj_t* obj : {screen, container_, content_}) {
            if (obj == nullptr) {
                continue;
            }
            lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(obj, OnPressed, LV_EVENT_PRESSED, this);
            lv_obj_add_event_cb(obj, OnClicked, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(obj, OnGesture, LV_EVENT_GESTURE, this);
        }

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

private:
    std::function<void()> wake_callback_;
    bool asleep_ = false;
    bool swallow_input_ = false;
    bool gesture_seen_ = false;  // a swipe has already been handled for this touch
    lv_obj_t* setting_box_ = nullptr;
    lv_obj_t* setting_aec_ = nullptr;
    lv_obj_t* setting_battery_ = nullptr;
    lv_obj_t* setting_clock_ = nullptr;
    // Screensaver: a full screen black plate created while asleep, with the
    // clock on top of it. Destroyed on wake.
    lv_obj_t* screensaver_box_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    int clock_minute_ = -1;  // minute currently shown, -1 forces a redraw
    int clock_x_ = -1;
    int clock_y_ = -1;

    static CustomLcdDisplay* Self(lv_event_t* e) {
        return static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
    }

    // A press anywhere brings the panel back from power save.
    static void OnPressed(lv_event_t* e) {
        auto* self = Self(e);
        if (self == nullptr) {
            return;
        }
        self->gesture_seen_ = false;  // a new touch starts here
        // Read the flag first: the wake callback clears it, which used to make
        // the swallow below never happen and every wake-up start a conversation.
        bool was_asleep = self->asleep_;
        if (self->wake_callback_) {
            self->wake_callback_();
        }
        if (was_asleep) {
            // This touch only woke the screen up, so drop the rest of it.
            self->swallow_input_ = true;
        }
    }

    static void OnClicked(lv_event_t* e) {
        auto* self = Self(e);
        if (self == nullptr) {
            return;
        }
        if (self->swallow_input_) {
            self->swallow_input_ = false;
            return;
        }
        // LVGL reports a click after a swipe as well. Only a real tap should
        // toggle the chat, otherwise every swipe would toggle it twice.
        if (self->gesture_seen_) {
            self->gesture_seen_ = false;
            return;
        }
        if (lv_obj_is_valid(self->setting_box_)) {
            return;  // taps are ignored while the settings panel is open
        }
        Application::GetInstance().ToggleChatState();
    }

    static void OnGesture(lv_event_t* e) {
        auto* self = Self(e);
        if (self == nullptr) {
            return;
        }
        self->gesture_seen_ = true;

        if (self->swallow_input_) {
            self->swallow_input_ = false;
            return;
        }

        // Only vertical swipes matter: either direction opens the settings
        // panel, and either direction closes it again. Requiring a particular
        // direction only made it easy to get wrong. Horizontal swipes belong to
        // the sliders inside the panel, so they are ignored here.
        auto dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) {
            return;
        }

        DisplayLockGuard lock(self);
        if (lv_obj_is_valid(self->setting_box_)) {
            self->CloseSettings();
        } else {
            self->OpenSettings();
        }
    }

    // ---- settings panel ---------------------------------------------------

    static void OnVolumeChanged(lv_event_t* e) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        auto volume = static_cast<int>(lv_slider_get_value(slider));
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume);
    }

    static void OnBrightnessChanged(lv_event_t* e) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        auto brightness = static_cast<uint8_t>(lv_slider_get_value(slider));
        auto backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(brightness);
        }
    }

    static void OnAecChanged(lv_event_t* e) {
        auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
        auto& app = Application::GetInstance();
        // Application::SetAecMode() schedules the change on the main task, so it
        // is safe to call from the LVGL task in any state. Bailing out when not
        // idle left the switch visually flipped but the mode unchanged.
        auto mode = lv_obj_has_state(sw, LV_STATE_CHECKED) ? kAecOnDeviceSide : kAecOff;
        app.SetAecMode(mode);
        Settings settings("audio", true);
        settings.SetInt("aec_mode", mode);
    }

    static lv_obj_t* MakeRow(lv_obj_t* parent) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        return row;
    }

    static lv_obj_t* AddSliderRow(lv_obj_t* parent, const char* text, int value, int min_value,
                                  lv_event_cb_t callback) {
        lv_obj_t* row = MakeRow(parent);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* slider = lv_slider_create(row);
        lv_obj_set_size(slider, lv_pct(62), 10);
        lv_obj_align(slider, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_slider_set_range(slider, min_value, 100);
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_set_ext_click_area(slider, 16);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, nullptr);
        return slider;
    }

    void OpenSettings() {
        if (lv_obj_is_valid(setting_box_)) {
            return;
        }
        auto theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = theme->text_font()->font();

        setting_box_ = lv_obj_create(lv_screen_active());
        // Kept deliberately smaller than the screen and slightly translucent so
        // the chat underneath stays readable while the panel is open.
        lv_obj_set_size(setting_box_, lv_pct(86), lv_pct(72));
        lv_obj_center(setting_box_);
        lv_obj_set_style_bg_color(setting_box_, lv_color_hex(0x101014), 0);
        lv_obj_set_style_bg_opa(setting_box_, LV_OPA_80, 0);
        lv_obj_set_style_radius(setting_box_, 18, 0);
        lv_obj_set_style_border_width(setting_box_, 0, 0);
        lv_obj_set_style_pad_all(setting_box_, 14, 0);
        lv_obj_set_flex_flow(setting_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(setting_box_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(setting_box_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(setting_box_);
        lv_obj_set_style_text_font(title, text_font, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_label_set_text(title, "设置");

        AddSliderRow(setting_box_, "音量",
                     Board::GetInstance().GetAudioCodec()->output_volume(), 0, OnVolumeChanged);

        auto backlight = Board::GetInstance().GetBacklight();
        // The panel has no backlight GPIO: brightness goes straight into the
        // AMOLED command 0x51, where 0 means a completely dark screen. Keep a
        // floor so the slider cannot make the device unusable.
        AddSliderRow(setting_box_, "亮度",
                     backlight != nullptr ? backlight->brightness() : 50, 5, OnBrightnessChanged);

#if CONFIG_USE_DEVICE_AEC
        // Voice interruption on/off, i.e. device side AEC. Only offered when
        // the build actually supports it.
        lv_obj_t* aec_row = MakeRow(setting_box_);
        lv_obj_t* aec_label = lv_label_create(aec_row);
        lv_label_set_text(aec_label, "语音打断");
        lv_obj_set_style_text_color(aec_label, lv_color_white(), 0);
        lv_obj_align(aec_label, LV_ALIGN_LEFT_MID, 0, 0);

        setting_aec_ = lv_switch_create(aec_row);
        lv_obj_set_size(setting_aec_, 50, 25);
        lv_obj_set_ext_click_area(setting_aec_, 12);
        lv_obj_align(setting_aec_, LV_ALIGN_RIGHT_MID, 0, 0);
        if (Application::GetInstance().GetAecMode() != kAecOff) {
            lv_obj_add_state(setting_aec_, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(setting_aec_, OnAecChanged, LV_EVENT_VALUE_CHANGED, nullptr);
#endif

        // Screensaver: a migrating clock, or a blank panel. Stored so the
        // choice survives a reboot.
        lv_obj_t* clock_row = MakeRow(setting_box_);
        lv_obj_t* clock_text = lv_label_create(clock_row);
        lv_label_set_text(clock_text, "息屏时钟");
        lv_obj_set_style_text_color(clock_text, lv_color_white(), 0);
        lv_obj_align(clock_text, LV_ALIGN_LEFT_MID, 0, 0);

        setting_clock_ = lv_switch_create(clock_row);
        lv_obj_set_size(setting_clock_, 50, 25);
        lv_obj_set_ext_click_area(setting_clock_, 12);
        lv_obj_align(setting_clock_, LV_ALIGN_RIGHT_MID, 0, 0);
        if (ScreensaverClockEnabled()) {
            lv_obj_add_state(setting_clock_, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(setting_clock_, OnClockChanged, LV_EVENT_VALUE_CHANGED, nullptr);

        // Battery level, straight from the AXP2101.
        setting_battery_ = lv_label_create(setting_box_);
        lv_obj_set_style_text_font(setting_battery_, text_font, 0);
        lv_obj_set_style_text_color(setting_battery_, lv_color_hex(0xB0B0B0), 0);
        RefreshBatteryLabel();
    }

    void CloseSettings() {
        if (lv_obj_is_valid(setting_box_)) {
            lv_obj_delete(setting_box_);
        }
        setting_box_ = nullptr;
        setting_aec_ = nullptr;
        setting_battery_ = nullptr;
        setting_clock_ = nullptr;
    }

public:
    // --------------------------------------------------------------- screensaver
    // Showing a slowly migrating clock instead of blanking the panel keeps the
    // time readable while spreading the wear over the whole display, so no
    // static image is burned in. When the user turns it off the panel is
    // switched off outright, which emits nothing at all.

    static constexpr int kClockInset = 60;  // stay clear of the rounded corners

    static bool ScreensaverClockEnabled() {
        Settings settings("display", false);
        return settings.GetBool("screensaver", true);
    }

    static void OnClockChanged(lv_event_t* e) {
        auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
        Settings settings("display", true);
        settings.SetBool("screensaver", lv_obj_has_state(sw, LV_STATE_CHECKED));
    }

    // The application already ticks UpdateStatusBar() once a second, so the
    // screensaver clock can ride on that instead of running a timer of its own.
    // Redraw only when the displayed minute actually changes, which is what the
    // stock status bar clock does and what keeps the phase from drifting.
    void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);

        if (!lv_obj_is_valid(clock_label_)) {
            return;
        }
        auto now = time(nullptr);
        tm tm_now = {};
        if (localtime_r(&now, &tm_now) == nullptr) {
            return;
        }
        int minute = tm_now.tm_hour * 60 + tm_now.tm_min;
        if (!update_all && minute == clock_minute_) {
            return;
        }
        DisplayLockGuard lock(this);
        clock_minute_ = minute;
        UpdateClockText();
        MoveClock();
    }

    void UpdateClockText() {
        if (!lv_obj_is_valid(clock_label_)) {
            return;
        }
        auto now = time(nullptr);
        tm tm_now = {};
        char text[16];
        // The clock is only set from the server once the device is online, so
        // guard against the 1970 epoch and show a placeholder until then.
        if (localtime_r(&now, &tm_now) == nullptr || tm_now.tm_year < 120) {
            lv_label_set_text(clock_label_, "--:--");
            return;
        }
        strftime(text, sizeof(text), "%H:%M", &tm_now);
        lv_label_set_text(clock_label_, text);
    }

    void MoveClock() {
        if (!lv_obj_is_valid(clock_label_)) {
            return;
        }
        lv_obj_update_layout(clock_label_);
        int max_x = LV_HOR_RES - lv_obj_get_width(clock_label_) - kClockInset;
        int max_y = LV_VER_RES - lv_obj_get_height(clock_label_) - kClockInset;
        if (max_x < kClockInset) {
            max_x = kClockInset;
        }
        if (max_y < kClockInset) {
            max_y = kClockInset;
        }
        int x = clock_x_;
        int y = clock_y_;
        // Keep moving away from the previous spot so the wear keeps spreading
        // instead of settling on a few favourite positions.
        for (int attempt = 0; attempt < 5; attempt++) {
            int nx = kClockInset + (int)(esp_random() % (uint32_t)(max_x - kClockInset + 1));
            int ny = kClockInset + (int)(esp_random() % (uint32_t)(max_y - kClockInset + 1));
            x = nx;
            y = ny;
            if (std::abs(nx - clock_x_) >= 60 || std::abs(ny - clock_y_) >= 60) {
                break;
            }
        }
        clock_x_ = x;
        clock_y_ = y;
        lv_obj_set_pos(clock_label_, x, y);
        lv_obj_move_foreground(clock_label_);
    }

    // The screensaver is the topmost input surface while the panel is dark, so
    // it owns the press that brings the device back. Passing input through to
    // whatever page happened to be open underneath let a clickable layer there
    // swallow the wake touch. Blank mode keeps the plate transparent but still
    // clickable, for the same reason.
    void ShowScreensaver(bool with_clock) {
        DisplayLockGuard lock(this);
        if (!lv_obj_is_valid(screensaver_box_)) {
            screensaver_box_ = lv_obj_create(lv_screen_active());
            lv_obj_remove_style_all(screensaver_box_);
            lv_obj_set_size(screensaver_box_, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_pos(screensaver_box_, 0, 0);
            lv_obj_add_flag(screensaver_box_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_remove_flag(screensaver_box_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(screensaver_box_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(screensaver_box_, OnPressed, LV_EVENT_PRESSED, this);
            lv_obj_add_event_cb(screensaver_box_, OnClicked, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(screensaver_box_, OnGesture, LV_EVENT_GESTURE, this);
        }
        // On an AMOLED a black pixel emits nothing, so an opaque plate hides the
        // chat ui without adding panel wear; only the digits light up.
        lv_obj_set_style_bg_color(screensaver_box_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screensaver_box_, with_clock ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_move_foreground(screensaver_box_);
        if (!with_clock) {
            // Panel off: the plate is invisible and only catches the wake touch.
            return;
        }
        if (!lv_obj_is_valid(clock_label_)) {
            // Parent it to the plate rather than the screen: the screen carries
            // a layout that would snap the label back to the top left corner.
            clock_label_ = lv_label_create(screensaver_box_);
            lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            auto theme = static_cast<LvglTheme*>(current_theme_);
            lv_obj_set_style_text_font(clock_label_, theme->text_font()->font(), 0);
            lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
            // A dark plate behind the digits keeps them readable whatever the
            // chat page happens to show underneath.
            lv_obj_set_style_bg_color(clock_label_, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(clock_label_, LV_OPA_60, 0);
            lv_obj_set_style_pad_all(clock_label_, 10, 0);
            lv_obj_set_style_radius(clock_label_, 12, 0);
            // Must stay transparent to input, otherwise it would swallow the
            // touch that is supposed to wake the panel.
            lv_obj_remove_flag(clock_label_, LV_OBJ_FLAG_CLICKABLE);
        }
        clock_x_ = -1;
        clock_y_ = -1;
        UpdateClockText();
        MoveClock();
        // Remember which minute is on screen so the per-second tick below only
        // redraws when it actually rolls over.
        auto now = time(nullptr);
        tm tm_now = {};
        clock_minute_ = (localtime_r(&now, &tm_now) != nullptr)
                            ? tm_now.tm_hour * 60 + tm_now.tm_min
                            : -1;
    }

    void HideScreensaver() {
        DisplayLockGuard lock(this);
        if (lv_obj_is_valid(clock_label_)) {
            lv_obj_delete(clock_label_);
        }
        clock_label_ = nullptr;
        if (lv_obj_is_valid(screensaver_box_)) {
            lv_obj_delete(screensaver_box_);
        }
        screensaver_box_ = nullptr;
    }

    // Panel power, i.e. the CO5300 DISPOFF/DISPON commands. Unlike a zero
    // backlight this stops the pixels emitting altogether, so nothing can burn
    // in. Everything else on the device keeps running.
    void SetPanelPower(bool on) {
        DisplayLockGuard lock(this);
        if (panel_ != nullptr) {
            esp_lcd_panel_disp_on_off(panel_, on);
        }
        if (on) {
            // The panel shows whatever is in its GRAM, so force a repaint to be
            // sure the first frame after waking is current.
            lv_obj_invalidate(lv_screen_active());
        }
    }

    void RefreshBatteryLabel() {
        if (!lv_obj_is_valid(setting_battery_)) {
            return;
        }
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            std::string text = "电量 " + std::to_string(level) + "%";
            if (charging) {
                text += (level >= 100) ? " (已充满)" : " (充电中)";
            } else if (!discharging) {
                text += " (未接电池)";
            }
            lv_label_set_text(setting_battery_, text.c_str());
        } else {
            lv_label_set_text(setting_battery_, "电量 --");
        }
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch16 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    Button key_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    bool screensaver_active_ = false;

    // Backlight level while the screensaver clock is showing. Low enough to be
    // unobtrusive at night and to keep the panel wear minimal.
    static constexpr int kScreensaverBrightness = 30;

    void SetDisplayAsleep(bool asleep) {
        if (display_ != nullptr) {
            static_cast<CustomLcdDisplay*>(display_)->SetAsleep(asleep);
        }
    }

    // Shared by the idle timer and the KEY button so both paths behave the same.
    void EnterScreensaver() {
        if (screensaver_active_ || display_ == nullptr) {
            return;
        }
        screensaver_active_ = true;
        SetDisplayAsleep(true);
        auto* display = static_cast<CustomLcdDisplay*>(display_);
        const bool with_clock = CustomLcdDisplay::ScreensaverClockEnabled();
        if (with_clock) {
            GetBacklight()->SetBrightness(kScreensaverBrightness);
        } else {
            GetBacklight()->SetBrightness(0);
            display->SetPanelPower(false);
        }
        // The plate goes up last so it is the topmost input surface.
        display->ShowScreensaver(with_clock);
    }

    void ExitScreensaver() {
        if (!screensaver_active_ || display_ == nullptr) {
            return;
        }
        screensaver_active_ = false;
        auto* display = static_cast<CustomLcdDisplay*>(display_);
        display->HideScreensaver();
        display->SetPanelPower(true);
        GetBacklight()->RestoreBrightness();
        SetDisplayAsleep(false);
    }

    void InitializePowerSaveTimer() {
        // Dim after a minute of idle, but never power off on our own.
        //
        // The timer is enabled unconditionally below and the stock 300s
        // shutdown therefore fires whether or not the board is on the
        // charger: the AXP2101 reports "discharging" even while charging
        // whenever the system load exceeds the charge current, so the device
        // switched itself off five minutes after being plugged in. Holding
        // PWR for four seconds still powers it off on demand.
        //
        // cpu_max_freq is -1, so "sleep" only dims the panel; it does not
        // enter light sleep and does not disturb USB.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() { EnterScreensaver(); });
        power_save_timer_->OnExitSleepMode([this]() { ExitScreensaver(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        // KEY: put the display to sleep straight away, and bring it back on the
        // next press. Waveshare documents this side button as GPIO 18.
        key_button_.OnClick([this]() {
            if (power_save_timer_ == nullptr) {
                return;
            }
            // Go through the timer so it records the state; otherwise WakeUp()
            // ignores later activity and the screen would stay dark.
            if (screensaver_active_) {
                power_save_timer_->WakeUp();
            } else {
                power_save_timer_->SleepNow();
            }
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        // Let a touch bring the panel back out of power save. The callback runs
        // in the LVGL task; PowerSaveTimer::WakeUp() only flips flags, and the
        // display lock is recursive, so calling it from there is safe.
        static_cast<CustomLcdDisplay*>(display_)->SetWakeCallback([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            // Idempotent: WakeUp() already goes through OnExitSleepMode when the
            // timer owns the sleep, but the KEY button can start one on its own.
            ExitScreensaver();
        });
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                // The panel reports its coordinates rotated by 180 degrees
                // relative to the LCD, so mirroring both axes put every touch
                // at the opposite corner and flipped swipe directions.
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch16()
        : boot_button_(BOOT_BUTTON_GPIO), key_button_(KEY_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        // A full pack on USB sits in standby: the AXP2101 reports neither
        // direction, which used to be indistinguishable from "no battery".
        // Charge-done is the signal that tells the two apart.
        charging = pmic_->IsCharging() || pmic_->IsChargingDone();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch16);
