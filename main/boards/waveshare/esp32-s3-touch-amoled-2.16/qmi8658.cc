#include "qmi8658.h"

#include <esp_log.h>

#define TAG "Qmi8658"

namespace {

constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl5 = 0x06;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegAccelXLow = 0x35;

constexpr uint8_t kChipId = 0x05;
// Address auto increment, so a six byte burst read works.
constexpr uint8_t kCtrl1AutoIncrement = 0x60;
// +/-4 g in the high nibble, 31.25 Hz in the low one.
constexpr uint8_t kCtrl2Range4gOdr31Hz = 0x18;
// Accelerometer low-pass filter on; the orientation math wants a calm signal.
constexpr uint8_t kCtrl5AccelLowPass = 0x01;
constexpr uint8_t kCtrl7AccelEnable = 0x01;

// 8192 counts per g in the +/-4 g range.
constexpr float kCountsPerG = 8192.0f;

constexpr int kTransferTimeoutMs = 20;
constexpr uint8_t kCandidateAddresses[] = {0x6B, 0x6A};

}  // namespace

Qmi8658::~Qmi8658() {
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
    }
}

bool Qmi8658::Write(uint8_t reg, uint8_t value) const {
    const uint8_t payload[] = {reg, value};
    return i2c_master_transmit(device_, payload, sizeof(payload), kTransferTimeoutMs) == ESP_OK;
}

bool Qmi8658::Read(uint8_t reg, uint8_t* data, size_t length) const {
    return i2c_master_transmit_receive(device_, &reg, 1, data, length, kTransferTimeoutMs) == ESP_OK;
}

bool Qmi8658::Begin(i2c_master_bus_handle_t bus) {
    if (bus == nullptr) {
        return false;
    }

    // The board does not document which side of the address jumper is fitted,
    // so try both and let WHO_AM_I decide.
    for (uint8_t address : kCandidateAddresses) {
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 400 * 1000;
        if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
            device_ = nullptr;
            continue;
        }

        uint8_t chip_id = 0;
        if (Read(kRegWhoAmI, &chip_id, 1) && chip_id == kChipId) {
            ESP_LOGI(TAG, "found at 0x%02x", address);
            break;
        }

        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }

    if (device_ == nullptr) {
        ESP_LOGW(TAG, "no QMI8658 on the shared I2C bus");
        return false;
    }

    // Same sequence the vendor demo uses: stop everything, configure, then
    // enable only the accelerometer.
    const bool configured = Write(kRegCtrl7, 0x00) &&
                            Write(kRegCtrl1, kCtrl1AutoIncrement) &&
                            Write(kRegCtrl2, kCtrl2Range4gOdr31Hz) &&
                            Write(kRegCtrl5, kCtrl5AccelLowPass) &&
                            Write(kRegCtrl7, kCtrl7AccelEnable);
    if (!configured) {
        ESP_LOGE(TAG, "configuration failed");
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }

    return true;
}

bool Qmi8658::ReadAccel(float* x, float* y, float* z) const {
    if (device_ == nullptr || x == nullptr || y == nullptr || z == nullptr) {
        return false;
    }

    uint8_t buffer[6] = {};
    if (!Read(kRegAccelXLow, buffer, sizeof(buffer))) {
        return false;
    }

    // Little endian, X then Y then Z.
    const int16_t raw_x = static_cast<int16_t>((buffer[1] << 8) | buffer[0]);
    const int16_t raw_y = static_cast<int16_t>((buffer[3] << 8) | buffer[2]);
    const int16_t raw_z = static_cast<int16_t>((buffer[5] << 8) | buffer[4]);

    *x = static_cast<float>(raw_x) / kCountsPerG;
    *y = static_cast<float>(raw_y) / kCountsPerG;
    *z = static_cast<float>(raw_z) / kCountsPerG;
    return true;
}
