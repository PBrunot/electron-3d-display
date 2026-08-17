#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#define LCD_HOST SPI2_HOST
#define PIN_MOSI gpio_num_t(41)
#define PIN_SCLK gpio_num_t(40)
#define PIN_CS gpio_num_t(39)
#define PIN_DC gpio_num_t(38)
#define PIN_RST gpio_num_t(42)
#define PIN_BL gpio_num_t(20)

// 80MHz, not e.g. 60MHz: the GP-SPI clock divides a fixed 80MHz peripheral clock by an
// integer only (80, 40, 26.7, 20 MHz, ...), so any non-achievable request silently snaps
// DOWN to the next achievable rate with no warning logged -- confirmed on-device via
// atom_view_test.cpp profiling: a 60MHz request measured ~24.2ms/frame SPI wait, matching
// the 40MHz math (23.0ms theoretical), not 60MHz's (~15.4ms). 80MHz is the only faster
// achievable step and exceeds the ST7789VW datasheet's Table 6 Tscycw spec (16ns min write
// cycle = 62.5MHz max) by 28% -- out-of-spec, needs on-hardware visual confirmation (watch
// for pixel glitches/noise), not assumed safe just because it's the next divisor.
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

auto Display::onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* userCtx) -> bool {
    Display* self = static_cast<Display*>(userCtx);
    BaseType_t highPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(self->s_flushDone, &highPriorityTaskWoken);
    return highPriorityTaskWoken == pdTRUE;
}

Display::Display(esp_lcd_panel_handle_t panel, uint16_t* frameBuf) : panel(panel), frameBuf(frameBuf) {}

Display::~Display() {
    if (frameBuf != nullptr)
        heap_caps_free(frameBuf);
    if (panel != nullptr)
        ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
}

Display::Display()
{
    this->s_flushDone = xSemaphoreCreateBinary();
    // Give it once up front: no frame is in flight yet, so waitForFlushDone() must not
    // block before the very first presentFrame() -- xSemaphoreCreateBinary() otherwise
    // starts empty (never given), which deadlocks any loop that waits before its first
    // present (e.g. flyOver()'s and the atom-view test loop's very first iteration).
    xSemaphoreGive(this->s_flushDone);

    gpio_config_t bl_cfg = {};
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    bl_cfg.pin_bit_mask = 1ULL << PIN_BL;
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_BL, 1);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_SCLK;
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = Display::kDisplayWidth * Display::kDisplayHeight * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = PIN_DC;
    io_config.cs_gpio_num = PIN_CS;
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = &Display::onColorTransDone;
    io_config.user_ctx = this;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    uint16_t* frame_buf =
        (uint16_t*)heap_caps_malloc(kDisplayWidth * kDisplayHeight * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (frame_buf == NULL) {
        ESP_LOGE(kDisplayTag, "failed to allocate frame buffer");
        abort();
    }

    this->panel = panel_handle;
    this->frameBuf = frame_buf;
}

auto Display::getFrameBuf() -> uint16_t* {
    return frameBuf;
}

void Display::presentFrame() {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(this->panel, 0, 0, Display::kDisplayWidth, Display::kDisplayHeight, this->frameBuf));
}

auto Display::waitForFlushDone() -> bool {
    return s_flushDone == nullptr || xSemaphoreTake(s_flushDone, portMAX_DELAY) == pdTRUE;
}