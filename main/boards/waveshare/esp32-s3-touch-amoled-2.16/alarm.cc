#include "alarm.h"

#include <utility>

#include <esp_log.h>
#include <esp_timer.h>

#include "settings.h"

#define TAG "Alarm"

namespace {

// Before 2024 the clock has never been set, so alarms must not fire.
constexpr int kEarliestValidYear = 2024;

constexpr int kRingSeconds = 60;
constexpr int kChunkSeconds = 5;

constexpr char kNamespace[] = "alarm";
constexpr char kCountKey[] = "count";
constexpr char kEntryKeys[Alarm::kMaxAlarms][3] = {"a0", "a1", "a2", "a3"};

}  // namespace

void Alarm::Init(RingHandler on_ring, StopHandler on_stop, ChangedHandler on_changed) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_ring_ = std::move(on_ring);
    on_stop_ = std::move(on_stop);
    on_changed_ = std::move(on_changed);
    Load();
}

void Alarm::Load() {
    Settings settings(kNamespace, false);
    count_ = settings.GetInt(kCountKey, 0);
    if (count_ < 0 || count_ > kMaxAlarms) {
        count_ = 0;
    }

    for (int i = 0; i < count_; i++) {
        const int32_t packed = settings.GetInt(kEntryKeys[i], 0);
        entries_[i].hour = packed & 0xFF;
        entries_[i].minute = (packed >> 8) & 0xFF;
        entries_[i].weekdays = (packed >> 16) & 0xFF;
        entries_[i].enabled = (packed >> 24) & 0x01;
        if (entries_[i].hour > 23 || entries_[i].minute > 59) {
            entries_[i].enabled = false;
        }
    }
    ESP_LOGI(TAG, "loaded %d alarm(s)", count_);
}

void Alarm::Store() {
    Settings settings(kNamespace, true);
    settings.SetInt(kCountKey, count_);
    for (int i = 0; i < kMaxAlarms; i++) {
        const Entry& entry = entries_[i];
        const int32_t packed = entry.hour | (entry.minute << 8) | (entry.weekdays << 16) |
                               (entry.enabled ? (1 << 24) : 0);
        settings.SetInt(kEntryKeys[i], i < count_ ? packed : 0);
    }
}

int Alarm::Add(int hour, int minute, uint8_t weekdays) {
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            return -1;
        }
        if (count_ >= kMaxAlarms) {
            return -1;
        }

        slot = count_++;
        entries_[slot].hour = static_cast<uint8_t>(hour);
        entries_[slot].minute = static_cast<uint8_t>(minute);
        entries_[slot].weekdays = weekdays & kEveryDay;
        entries_[slot].enabled = true;
        last_fired_[slot] = kNeverFired;
        Store();
    }

    ESP_LOGI(TAG, "added %02d:%02d mask=0x%02x in slot %d", hour, minute, weekdays, slot);
    if (on_changed_) {
        on_changed_();
    }
    return slot;
}

bool Alarm::RemoveLocked(int index) {
    if (index < 0 || index >= count_) {
        return false;
    }
    if (ringing_ && ringing_index_ == index) {
        StopRingingLocked();
    }
    for (int i = index; i + 1 < count_; i++) {
        entries_[i] = entries_[i + 1];
        last_fired_[i] = last_fired_[i + 1];
    }
    entries_[count_ - 1] = Entry();
    last_fired_[count_ - 1] = kNeverFired;
    count_--;
    // The ring's index shifted with the list.
    if (ringing_ && ringing_index_ > index) {
        --ringing_index_;
    }
    Store();
    return true;
}

bool Alarm::Remove(int index) {
    bool stopped = false;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped = ringing_ && ringing_index_ == index;
        removed = RemoveLocked(index);
    }
    if (!removed) {
        return false;
    }
    if (stopped && on_stop_) {
        on_stop_();
    }
    if (on_changed_) {
        on_changed_();
    }
    return true;
}

bool Alarm::SetEnabled(int index, bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= count_) {
            return false;
        }
        entries_[index].enabled = enabled;
        last_fired_[index] = kNeverFired;
        Store();
    }
    if (on_changed_) {
        on_changed_();
    }
    return true;
}

bool Alarm::IsEnabled(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index >= 0 && index < count_ && entries_[index].enabled;
}

Alarm::Entry Alarm::Get(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= count_) {
        return Entry();
    }
    return entries_[index];
}

int Alarm::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool Alarm::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

bool Alarm::ringing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringing_;
}

uint8_t Alarm::WeekdayBit(const tm& local_tm) {
    // tm_wday is 0 = Sunday; the mask starts at Monday.
    return static_cast<uint8_t>(1u << ((local_tm.tm_wday + 6) % 7));
}

uint32_t Alarm::MinuteStamp(const tm& local_tm) {
    return static_cast<uint32_t>(local_tm.tm_yday) * 1440u +
           static_cast<uint32_t>(local_tm.tm_hour) * 60u + static_cast<uint32_t>(local_tm.tm_min);
}

int Alarm::FindDue(const tm& local_tm, uint32_t stamp) const {
    const uint8_t today = WeekdayBit(local_tm);
    for (int i = 0; i < count_; i++) {
        const Entry& entry = entries_[i];
        if (!entry.enabled) {
            continue;
        }
        if (entry.hour != local_tm.tm_hour || entry.minute != local_tm.tm_min) {
            continue;
        }
        // A zero mask means one-shot and matches whatever day it happens to be.
        if (entry.weekdays != 0 && (entry.weekdays & today) == 0) {
            continue;
        }
        if (last_fired_[i] == stamp) {
            continue;
        }
        return i;
    }
    return -1;
}

void Alarm::Tick(time_t now) {
    bool fire = false;
    bool first = false;
    bool stopped = false;
    bool changed = false;
    Entry fired;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        tm local_tm = {};
        if (localtime_r(&now, &local_tm) == nullptr) {
            return;
        }

        // Monotonic, so a wall-clock jump cannot stretch the ring window.
        const int64_t now_us = esp_timer_get_time();
        const uint32_t stamp = MinuteStamp(local_tm);

        if (ringing_) {
            if (now_us - ring_started_us_ >= static_cast<int64_t>(kRingSeconds) * 1000000) {
                StopRingingLocked();
                stopped = true;
            } else {
                if (now_us - last_chunk_us_ >= static_cast<int64_t>(kChunkSeconds) * 1000000) {
                    last_chunk_us_ = now_us;
                    fire = true;
                    fired = ringing_entry_;
                }
                // Don't lose an alarm that falls due during this ring.
                if (pending_index_ < 0) {
                    pending_index_ = FindDue(local_tm, stamp);
                }
            }
        } else if (local_tm.tm_year + 1900 >= kEarliestValidYear) {
            int due = pending_index_;
            pending_index_ = -1;
            if (due >= count_ || !entries_[due].enabled) {
                due = -1;
            }
            if (due < 0) {
                due = FindDue(local_tm, stamp);
            }

            if (due >= 0) {
                const Entry& entry = entries_[due];
                last_fired_[due] = stamp;
                const bool one_shot = (entry.weekdays == 0);
                fired = entry;
                first = true;
                fire = true;

                // Must run before ringing_ is set, or Remove() stops the ring.
                if (one_shot) {
                    RemoveLocked(due);
                    changed = true;
                    ringing_index_ = -1;
                } else {
                    ringing_index_ = due;
                }
                ringing_ = true;
                ring_started_us_ = now_us;
                last_chunk_us_ = now_us;
            }
        }
    }

    // Callbacks run with the lock released.
    if (stopped) {
        ESP_LOGI(TAG, "ring window closed");
        if (on_stop_) {
            on_stop_();
        }
        return;
    }
    if (changed && on_changed_) {
        on_changed_();
    }
    if (fire && on_ring_) {
        if (first) {
            ESP_LOGI(TAG, "ringing at %02d:%02d", fired.hour, fired.minute);
        }
        on_ring_(fired, first);
    }
}

void Alarm::StopRingingLocked() {
    ringing_ = false;
    ringing_index_ = -1;
}

bool Alarm::StopRinging() {
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ringing_) {
            StopRingingLocked();
            stopped = true;
        }
    }
    if (stopped && on_stop_) {
        on_stop_();
    }
    return stopped;
}
