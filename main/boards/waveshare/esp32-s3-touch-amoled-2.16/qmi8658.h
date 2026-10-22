#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>

// Minimal QMI8658 driver: the accelerometer is all the screen orientation
// needs, so the gyroscope is left switched off to keep each sample cheap.
class Qmi8658 {
public:
    Qmi8658() = default;
    ~Qmi8658();

    Qmi8658(const Qmi8658&) = delete;
    Qmi8658& operator=(const Qmi8658&) = delete;

    // Probes 0x6B then 0x6A, checks WHO_AM_I and configures +/-4 g at 31.25 Hz.
    bool Begin(i2c_master_bus_handle_t bus);

    // Gravity in g, in the sensor's own frame. False when the bus fails.
    bool ReadAccel(float* x, float* y, float* z) const;

private:
    bool Write(uint8_t reg, uint8_t value) const;
    bool Read(uint8_t reg, uint8_t* data, size_t length) const;

    i2c_master_dev_handle_t device_ = nullptr;
};
