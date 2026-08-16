#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* kDisplayTag = "display";

// Given by onColorTransDone() (an ISR callback -- see esp_lcd_panel_io_spi_config_t's
// on_color_trans_done, fired exactly once per presentFrame() call, on its true last DMA
// chunk) and taken by presentFrame() itself, so presentFrame() only returns once the
// previous transfer has genuinely finished, not just been queued -- see display.h's
// presentFrame() doc comment for why this matters.
static SemaphoreHandle_t s_flushDone = nullptr;

static bool onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t highPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_flushDone, &highPriorityTaskWoken);
    return highPriorityTaskWoken == pdTRUE;
}

#define LCD_HOST SPI2_HOST
#define PIN_MOSI gpio_num_t(41)
#define PIN_SCLK gpio_num_t(40)
#define PIN_CS gpio_num_t(39)
#define PIN_DC gpio_num_t(38)
#define PIN_RST gpio_num_t(42)
#define PIN_BL gpio_num_t(20)

#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

Display initDisplay() {
    s_flushDone = xSemaphoreCreateBinary();

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
    buscfg.max_transfer_sz = kDisplayWidth * kDisplayHeight * sizeof(uint16_t);
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
    io_config.on_color_trans_done = onColorTransDone;
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

    return Display{panel_handle, frame_buf};
}

void presentFrame(const Display& d) {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(d.panel, 0, 0, kDisplayWidth, kDisplayHeight, d.frameBuf));
    xSemaphoreTake(s_flushDone, portMAX_DELAY);
}
