#include "ux/imu.h"

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/board.h" // kIsCYD
#include "config/hardware_constants.h"

static const char *kImuTag = "imu";

static constexpr auto kPinSda = gpio_num_t(47);
static constexpr auto kPinScl = gpio_num_t(48);

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
    if constexpr (kIsCYD)
    {
        // CYD has no QMI8658 and no wiring for one -- see imu.h's note. Deliberately skip I2C
        // bus bring-up entirely rather than attempting it with the S3's pins (47/48 aren't even
        // valid GPIO numbers on plain ESP32, which only goes up to 39).
        ESP_LOGI(kImuTag, "no IMU on this target (CYD) -- Qmi8658 is a no-op, readAccelG() always fails");
        return;
    }
    else
    {
        i2c_master_bus_config_t busCfg = {};
        busCfg.i2c_port = I2C_NUM_0;
        busCfg.sda_io_num = kPinSda;
        busCfg.scl_io_num = kPinScl;
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
            !writeReg(kRegCtrl3, uint8_t((kGyroRange512dpsBits << 4) | kOdr250HzBits)) ||
            !writeReg(kRegCtrl7, kCtrl7AccelGyroEnabled))
        {
            ESP_LOGE(kImuTag, "failed to configure accelerometer/gyroscope (CTRL1/CTRL2/CTRL3/CTRL7 write)");
            abort();
        }
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
    if constexpr (kIsCYD)
    {
        (void)outX;
        (void)outY;
        (void)outZ;
        return false;
    }
    else
    {
        uint8_t d[6];
        if (!readRegs(kRegAccelOut, d, sizeof(d)))
            return false;
        *outX = orb_real_t(signed16(d[0], d[1])) / kRange4gScale;
        *outY = orb_real_t(signed16(d[2], d[3])) / kRange4gScale;
        *outZ = orb_real_t(signed16(d[4], d[5])) / kRange4gScale;
        return true;
    }
}

bool Qmi8658::readAccelGyro(orb_real_t *outAx, orb_real_t *outAy, orb_real_t *outAz, orb_real_t *outGx,
                            orb_real_t *outGy, orb_real_t *outGz)
{
    if constexpr (kIsCYD)
    {
        (void)outAx;
        (void)outAy;
        (void)outAz;
        (void)outGx;
        (void)outGy;
        (void)outGz;
        return false;
    }
    else
    {
        uint8_t d[12];
        if (!readRegs(kRegAccelOut, d, sizeof(d)))
            return false;
        *outAx = orb_real_t(signed16(d[0], d[1])) / kRange4gScale;
        *outAy = orb_real_t(signed16(d[2], d[3])) / kRange4gScale;
        *outAz = orb_real_t(signed16(d[4], d[5])) / kRange4gScale;
        *outGx = orb_real_t(signed16(d[6], d[7])) / kGyroScale512dps;
        *outGy = orb_real_t(signed16(d[8], d[9])) / kGyroScale512dps;
        *outGz = orb_real_t(signed16(d[10], d[11])) / kGyroScale512dps;
        return true;
    }
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
    orb_real_t magDelta = std::abs(mag - defaultMag);
    bool planar = similarity >= kPlanarMinSimilarity && magDelta <= kPlanarMaxMagnitudeDeltaG;
    ESP_LOGI(kImuTag, "planar check: reading=(%.3f,%.3f,%.3f)g similarity=%.4f magDelta=%.3fg -> %s", double(x),
             double(y), double(z), double(similarity), double(magDelta), planar ? "PLANAR" : "not planar");
    return planar;
}
