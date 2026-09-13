#include "ux/ft6336g.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/board.h" // kBoard
#include "config/hardware_constants.h"

static const char *kFt6336Tag = "ft6336g";

static constexpr auto kPinSda = gpio_num_t(16);
static constexpr auto kPinScl = gpio_num_t(15);
static constexpr auto kPinRst = gpio_num_t(18);

// FocalTech FT6x36-family register map (see this file's header comment) -- TD_STATUS's low
// nibble is the touch-point count (0 or 1 on this single-touch usage), TOUCH1_XH/YH's low
// nibble is the coordinate's high 4 bits, TOUCH1_XL/YL the low 8 bits.
static constexpr uint8_t kRegTdStatus = 0x02;

Ft6336g::Ft6336g()
{
    if constexpr (kBoard != Board::kES3C28P)
    {
        // No FT6336G on this board -- see ft6336g.h's file comment.
        ESP_LOGI(kFt6336Tag,
                 "no capacitive touch panel on this board -- Ft6336g is a no-op, isTouched()/readRaw() always false");
        return;
    }
    else
    {
        gpio_config_t rstCfg = {};
        rstCfg.mode = GPIO_MODE_OUTPUT;
        rstCfg.pin_bit_mask = 1ULL << kPinRst;
        gpio_config(&rstCfg);

        // Hardware reset pulse before I2C traffic -- no FT6336G-specific timing figure
        // available (this repo has no datasheet for the exact chip on this board, see file
        // comment); these durations follow the common shape used by third-party FT6x36
        // drivers (a short low pulse, then a longer settle delay), UNVERIFIED against this
        // exact part.
        gpio_set_level(kPinRst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(kPinRst, 1);
        vTaskDelay(pdMS_TO_TICKS(300));

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
        devCfg.device_address = kFt6336Addr;
        devCfg.scl_speed_hz = kFt6336I2cFreqHz;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_, &devCfg, &dev_));

        ESP_LOGI(kFt6336Tag, "FT6336G capacitive touch ready (SDA=16 SCL=15 RST=18, addr 0x%02x)", kFt6336Addr);
    }
}

Ft6336g::~Ft6336g()
{
    if (dev_ != nullptr)
        i2c_master_bus_rm_device(dev_);
    if (bus_ != nullptr)
        i2c_del_master_bus(bus_);
}

bool Ft6336g::readTouch1(uint16_t *outX, uint16_t *outY) const
{
    if (dev_ == nullptr)
        return false;
    uint8_t buf[5] = {}; // TD_STATUS, TOUCH1_XH, TOUCH1_XL, TOUCH1_YH, TOUCH1_YL
    uint8_t reg = kRegTdStatus;
    if (i2c_master_transmit_receive(dev_, &reg, 1, buf, sizeof(buf), kFt6336XferTimeoutMs) != ESP_OK)
        return false;
    if ((buf[0] & 0x0F) == 0) // no touch point currently reported
        return false;
    *outX = uint16_t(((buf[1] & 0x0F) << 8) | buf[2]);
    *outY = uint16_t(((buf[3] & 0x0F) << 8) | buf[4]);
    return true;
}

bool Ft6336g::isTouched() const
{
    if constexpr (kBoard != Board::kES3C28P)
        return false;
    else
    {
        if (dev_ == nullptr)
            return false;
        uint8_t reg = kRegTdStatus;
        uint8_t status = 0;
        if (i2c_master_transmit_receive(dev_, &reg, 1, &status, 1, kFt6336XferTimeoutMs) != ESP_OK)
            return false;
        return (status & 0x0F) != 0;
    }
}

bool Ft6336g::readRaw(uint16_t *outX, uint16_t *outY)
{
    if constexpr (kBoard != Board::kES3C28P)
    {
        (void)outX;
        (void)outY;
        return false;
    }
    else
    {
        return readTouch1(outX, outY);
    }
}
