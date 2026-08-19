#include "imu.h"

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tilt_defaults.h"

static const char *kImuTag = "imu";

// Averaged over kPlanarCheckSamples rather than a single reading, so a momentary bump right
// at power-on doesn't cause a false "not planar".
constexpr int kPlanarCheckSamples = 20;
constexpr uint32_t kPlanarCheckSampleDelayMs = 8;
// Cosine-similarity floor against the hardcoded default baseline -- see checkPlanarAtBoot()'s
// doc comment (imu.h) for why this must be much stricter than tilt_gesture.h's
// cfg.minDirectionSimilarity.
constexpr orb_real_t kPlanarMinSimilarity = orb_real_t(0.995);
// Guards against a magnitude far from 1g (board in free-fall/being handled) reading as
// "planar" just because its direction happens to line up.
constexpr orb_real_t kPlanarMaxMagnitudeDeltaG = orb_real_t(0.15);

#define PIN_SDA gpio_num_t(47)
#define PIN_SCL gpio_num_t(48)

constexpr uint16_t kImuAddr = 0x6B;
constexpr uint32_t kImuI2cFreqHz = 400000;
constexpr int kImuXferTimeoutMs = 1000;

constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegAccelOut = 0x35; // AX_L, 6 bytes: AX/AY/AZ, little-endian, 2's complement

constexpr uint8_t kExpectedWhoAmI = 0x05;

// CTRL2 = accel range in bits [6:4] | ODR in bits [3:0] -- see qmi8658.py's header comment.
constexpr uint8_t kRange4gBits = 1;
constexpr uint8_t kOdr250HzBits = 5;
constexpr orb_real_t kRange4gScale = orb_real_t(8192.0); // LSB/g at +-4g full scale

static int16_t signed16(uint8_t lo, uint8_t hi)
{
    return int16_t(uint16_t(lo) | (uint16_t(hi) << 8));
}

bool Qmi8658::writeReg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(dev_, payload, sizeof(payload), kImuXferTimeoutMs) == ESP_OK;
}

bool Qmi8658::readRegs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev_, &reg, 1, buf, len, kImuXferTimeoutMs) == ESP_OK;
}

Qmi8658::Qmi8658()
{
    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = I2C_NUM_0;
    busCfg.sda_io_num = PIN_SDA;
    busCfg.scl_io_num = PIN_SCL;
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&busCfg, &bus_));

    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = kImuAddr;
    devCfg.scl_speed_hz = kImuI2cFreqHz;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_, &devCfg, &dev_));

    uint8_t who = 0;
    if (!readRegs(kRegWhoAmI, &who, 1) || who != kExpectedWhoAmI)
    {
        ESP_LOGE(kImuTag, "WHO_AM_I mismatch: got 0x%02x, expected 0x%02x", who, kExpectedWhoAmI);
        abort();
    }
    ESP_LOGI(kImuTag, "QMI8658 found at 0x%02x, WHO_AM_I=0x%02x", kImuAddr, who);

    if (!writeReg(kRegCtrl1, 0x60) || !writeReg(kRegCtrl2, uint8_t((kRange4gBits << 4) | kOdr250HzBits)) ||
        !writeReg(kRegCtrl7, 0x01))
    {
        ESP_LOGE(kImuTag, "failed to configure accelerometer (CTRL1/CTRL2/CTRL7 write)");
        abort();
    }
}

Qmi8658::~Qmi8658()
{
    if (dev_ != nullptr)
        i2c_master_bus_rm_device(dev_);
    if (bus_ != nullptr)
        i2c_del_master_bus(bus_);
}

bool Qmi8658::readAccelG(orb_real_t *outX, orb_real_t *outY, orb_real_t *outZ)
{
    uint8_t d[6];
    if (!readRegs(kRegAccelOut, d, sizeof(d)))
        return false;
    *outX = orb_real_t(signed16(d[0], d[1])) / kRange4gScale;
    *outY = orb_real_t(signed16(d[2], d[3])) / kRange4gScale;
    *outZ = orb_real_t(signed16(d[4], d[5])) / kRange4gScale;
    return true;
}

bool Qmi8658::checkPlanarAtBoot()
{
    orb_real_t sumX = orb_real_t(0), sumY = orb_real_t(0), sumZ = orb_real_t(0);
    int ok = 0;
    for (int i = 0; i < kPlanarCheckSamples; i++)
    {
        orb_real_t x, y, z;
        if (readAccelG(&x, &y, &z))
        {
            sumX += x;
            sumY += y;
            sumZ += z;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(kPlanarCheckSampleDelayMs));
    }
    if (ok == 0)
    {
        ESP_LOGW(kImuTag, "planar check: no IMU samples read, assuming not planar");
        return false;
    }

    orb_real_t x = sumX / orb_real_t(ok), y = sumY / orb_real_t(ok), z = sumZ / orb_real_t(ok);
    orb_real_t mag = std::sqrt(x * x + y * y + z * z);
    orb_real_t defaultMag = std::sqrt(kDefaultBaselineX * kDefaultBaselineX + kDefaultBaselineY * kDefaultBaselineY +
                                      kDefaultBaselineZ * kDefaultBaselineZ);
    if (mag < orb_real_t(1e-6))
    {
        ESP_LOGW(kImuTag, "planar check: near-zero reading, assuming not planar");
        return false;
    }

    orb_real_t similarity =
        (x * kDefaultBaselineX + y * kDefaultBaselineY + z * kDefaultBaselineZ) / (mag * defaultMag);
    orb_real_t magDelta = mag > defaultMag ? mag - defaultMag : defaultMag - mag;
    bool planar = similarity >= kPlanarMinSimilarity && magDelta <= kPlanarMaxMagnitudeDeltaG;
    ESP_LOGI(kImuTag, "planar check: reading=(%.3f,%.3f,%.3f)g similarity=%.4f magDelta=%.3fg -> %s", double(x),
             double(y), double(z), double(similarity), double(magDelta), planar ? "PLANAR" : "not planar");
    return planar;
}
