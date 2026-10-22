#include "auto_rotation.h"

#include <cmath>

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "AutoRotate"

namespace auto_rotation {

namespace {

constexpr int kSampleIntervalMs = 100;
constexpr int64_t kSettleUs = 500 * 1000;      // hold a pose this long to act
constexpr int64_t kCooldownUs = 1000 * 1000;   // never flip faster than this

// The sensor is mounted 90 degrees round relative to the panel: with no
// correction every orientation came out a quarter turn off.
constexpr int kMountingOffset = 90;

}  // namespace

int ClassifyPose(float x, float y, float z) {
    const float magnitude = x * x + y * y + z * z;
    const float dominant = std::fmax(std::fabs(x), std::fabs(y));
    const float minor = std::fmin(std::fabs(x), std::fabs(y));

    // Reject every pose where the answer would be a guess: being shaken or in
    // free fall, lying flat, or sitting close to a 45 degree diagonal where the
    // two axes disagree. Keeping the current orientation beats flickering.
    if (magnitude < 0.55f || magnitude > 1.65f || dominant < 0.60f ||
        std::fabs(z) > 0.80f || dominant < minor * 1.40f) {
        return -1;
    }

    if (std::fabs(y) >= std::fabs(x)) {
        return y > 0 ? 0 : 180;
    }
    return x > 0 ? 270 : 90;
}

int ScreenAngle(int pose, int offset) {
    return (360 - (pose + offset) % 360) % 360;
}

Controller::~Controller() {
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
}

void Controller::Start(AngleHandler on_angle, PauseHandler paused,
                       i2c_master_bus_handle_t bus) {
    if (task_ != nullptr) {
        return;
    }
    on_angle_ = std::move(on_angle);
    paused_ = std::move(paused);

    if (!imu_.Begin(bus)) {
        ESP_LOGW(TAG, "sensor unavailable, automatic rotation stays off");
        return;
    }

    // Small stack and a low priority: this wakes up ten times a second, reads
    // six bytes and goes back to sleep.
    if (xTaskCreate(TaskEntry, "auto_rotate", 3072, this, 2, &task_) != pdPASS) {
        ESP_LOGE(TAG, "could not start the task");
        task_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "ready");
}

void Controller::TaskEntry(void* arg) {
    static_cast<Controller*>(arg)->Run();
    // Run() never returns; this keeps the compiler from complaining.
    vTaskDelete(nullptr);
}

void Controller::Run() {
    int candidate = -1;
    int applied = -1;
    int64_t candidate_since = 0;
    int64_t last_change = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));

        if (!on_angle_ || (paused_ && paused_())) {
            candidate = -1;
            continue;
        }

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!imu_.ReadAccel(&x, &y, &z)) {
            continue;
        }

        const int pose = ClassifyPose(x, y, z);
        if (pose < 0) {
            candidate = -1;
            continue;
        }

        const int angle = ScreenAngle(pose, kMountingOffset);
        const int64_t now = esp_timer_get_time();
        if (angle != candidate) {
            // A new pose. Start its settle window and wait for the next sample.
            candidate = angle;
            candidate_since = now;
            continue;
        }
        if (now - candidate_since < kSettleUs || now - last_change < kCooldownUs ||
            angle == applied) {
            continue;
        }

        on_angle_(angle);
        applied = angle;
        last_change = now;
    }
}

}  // namespace auto_rotation
