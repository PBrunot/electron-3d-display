#include "ux/touch.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h" // esp_rom_delay_us
#include "config/board.h" // kIsCYD

static const char *kTouchTag = "touch";

static constexpr auto kPinClk = gpio_num_t(25);
static constexpr auto kPinMosi = gpio_num_t(32);
static constexpr auto kPinCs = gpio_num_t(33);
static constexpr auto kPinIrq = gpio_num_t(36);
static constexpr auto kPinMiso = gpio_num_t(39);

// XPT2046 control-byte channel selects: start bit + A2A1A0 + MODE(12-bit) + SER/DFR(differential)
// + PD1PD0(power-down between conversions) -- the same values used by essentially every
// XPT2046 driver (e.g. PaulStoffregen/XPT2046_Touchscreen's READ_X/READ_Y constants).
static constexpr uint8_t kCmdReadX = 0xD0;
static constexpr uint8_t kCmdReadY = 0x90;

// Half-period delay per bit-banged clock edge. The XPT2046 tolerates a clock well under 2MHz;
// this is a small fraction of that, with ample margin, so exact timing doesn't matter here.
static constexpr int kClockHalfPeriodUs = 2;

Xpt2046::Xpt2046()
{
    if constexpr (kIsCYD)
    {
        gpio_config_t outCfg = {};
        outCfg.pin_bit_mask = (1ULL << kPinClk) | (1ULL << kPinMosi) | (1ULL << kPinCs);
        outCfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&outCfg);

        gpio_config_t inCfg = {};
        inCfg.pin_bit_mask = (1ULL << kPinMiso) | (1ULL << kPinIrq);
        inCfg.mode = GPIO_MODE_INPUT;
        // GPIO34-39 are input-only pads with no internal pull resistor on the classic ESP32, so
        // GPIO_PULLUP_ENABLE here just logs "GPIO number error" twice and does nothing --
        // confirmed on real hardware that the CYD board already pulls IRQ high externally
        // (isTouched() works fine idle-high/low-when-pressed without it).
        inCfg.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&inCfg);

        gpio_set_level(kPinCs, 1);
        gpio_set_level(kPinClk, 0);
        ESP_LOGI(kTouchTag, "XPT2046 bit-banged touch ready (CLK=25 MOSI=32 CS=33 IRQ=36 MISO=39)");
    }
    else
    {
        // No XPT2046 on this target -- see touch.h's file comment.
        ESP_LOGI(kTouchTag, "no touch panel on this target -- Xpt2046 is a no-op, isTouched()/readRaw() always false");
    }
}

bool Xpt2046::isTouched() const
{
    if constexpr (kIsCYD)
        return gpio_get_level(kPinIrq) == 0;
    else
        return false;
}

uint16_t Xpt2046::xferCmd(uint8_t cmd) const
{
    if constexpr (kIsCYD)
    {
        gpio_set_level(kPinCs, 0);

        for (int i = 7; i >= 0; i--)
        {
            gpio_set_level(kPinMosi, (cmd >> i) & 1);
            esp_rom_delay_us(kClockHalfPeriodUs);
            gpio_set_level(kPinClk, 1);
            esp_rom_delay_us(kClockHalfPeriodUs);
            gpio_set_level(kPinClk, 0);
        }

        // 16 more clocks: a leading null bit, the 12-bit result (MSB first), then 3 trailing
        // zero-pad bits -- shifting the accumulated 16 bits right by 3 leaves the 12-bit sample.
        uint16_t raw = 0;
        for (int i = 0; i < 16; i++)
        {
            esp_rom_delay_us(kClockHalfPeriodUs);
            gpio_set_level(kPinClk, 1);
            esp_rom_delay_us(kClockHalfPeriodUs);
            raw = uint16_t((raw << 1) | (gpio_get_level(kPinMiso) & 1));
            gpio_set_level(kPinClk, 0);
        }

        gpio_set_level(kPinCs, 1);
        return uint16_t(raw >> 3);
    }
    else
    {
        (void)cmd;
        return 0;
    }
}

bool Xpt2046::readRaw(uint16_t *outX, uint16_t *outY)
{
    if constexpr (kIsCYD)
    {
        if (!isTouched())
            return false;
        uint16_t x = xferCmd(kCmdReadX);
        uint16_t y = xferCmd(kCmdReadY);
        if (!isTouched()) // lifted mid-transaction -- discard the torn sample rather than return it
            return false;
        *outX = x;
        *outY = y;
        return true;
    }
    else
    {
        (void)outX;
        (void)outY;
        return false;
    }
}
