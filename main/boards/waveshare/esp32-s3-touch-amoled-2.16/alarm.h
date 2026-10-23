#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>

class Alarm {
public:
    static constexpr int kMaxAlarms = 4;
    // bit 0 = Monday ... bit 6 = Sunday.
    static constexpr uint8_t kEveryDay = 0x7F;
    static constexpr uint32_t kNeverFired = 0xFFFFFFFFu;

    struct Entry {
        uint8_t hour = 0;
        uint8_t minute = 0;
        // 0 = one-shot: fires once, then removes itself.
        uint8_t weekdays = 0;
        bool enabled = false;
    };

    // `first` is true only at the start of a ring, not on the repeats.
    using RingHandler = std::function<void(const Entry&, bool first)>;
    using StopHandler = std::function<void()>;
    // One-shots remove themselves from inside Tick(), so the UI must be told.
    using ChangedHandler = std::function<void()>;

    // Safe to call from any task.
    void Init(RingHandler on_ring, StopHandler on_stop, ChangedHandler on_changed);

    int Add(int hour, int minute, uint8_t weekdays);  // slot, or -1 when full/invalid
    bool Remove(int index);
    bool SetEnabled(int index, bool enabled);
    bool IsEnabled(int index) const;

    int Count() const;
    Entry Get(int index) const;
    bool Empty() const;

    // Call once a second. Nothing fires before the clock has been set.
    void Tick(time_t now);

    bool ringing() const;
    bool StopRinging();

private:
    void Load();
    void Store();
    bool RemoveLocked(int index);
    void StopRingingLocked();
    static uint8_t WeekdayBit(const tm& local_tm);
    static uint32_t MinuteStamp(const tm& local_tm);
    int FindDue(const tm& local_tm, uint32_t stamp) const;  // caller holds mutex_

    Entry entries_[kMaxAlarms];
    int count_ = 0;
    uint32_t last_fired_[kMaxAlarms] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};

    bool ringing_ = false;
    int ringing_index_ = -1;
    int pending_index_ = -1;  // fell due during another ring
    // Monotonic, so a wall-clock jump cannot stretch the ring window.
    int64_t ring_started_us_ = 0;
    int64_t last_chunk_us_ = 0;
    // The list index is stale once a one-shot removes itself.
    Entry ringing_entry_;

    mutable std::mutex mutex_;

    RingHandler on_ring_;
    StopHandler on_stop_;
    ChangedHandler on_changed_;
};
