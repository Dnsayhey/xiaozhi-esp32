#pragma once

#include <functional>

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "qmi8658.h"

namespace auto_rotation {

// Pose of the device as seen by the sensor: 0, 90, 180 or 270 degrees, or -1
// when the answer would be a guess (in motion, lying flat, near a diagonal).
int ClassifyPose(float x, float y, float z);

// Turns a pose into a screen angle. The sensor is mounted at its own angle
// relative to the panel, so the mounting correction lives in `offset`.
int ScreenAngle(int pose, int offset);

// Watches the accelerometer and asks for a new orientation once the device has
// been held still in a new pose for long enough. It knows nothing about the
// display: the callbacks keep it independent of who owns the screen.
class Controller {
public:
    using AngleHandler = std::function<void(int degrees)>;
    // True while rotating would be pointless or unwanted: screen off, or the
    // user has automatic rotation switched off.
    using PauseHandler = std::function<bool()>;

    Controller() = default;
    ~Controller();

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    void Start(AngleHandler on_angle, PauseHandler paused, i2c_master_bus_handle_t bus);

private:
    static void TaskEntry(void* arg);
    void Run();

    Qmi8658 imu_;
    AngleHandler on_angle_;
    PauseHandler paused_;
    TaskHandle_t task_ = nullptr;
};

}  // namespace auto_rotation
