#include "display.h"

#include <cstring>

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

auto Display::onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx) -> bool
{
    Display *self = static_cast<Display *>(userCtx);
    BaseType_t highPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(self->s_flushDone, &highPriorityTaskWoken);
    return highPriorityTaskWoken == pdTRUE;
}

Display::Display(esp_lcd_panel_handle_t panel, uint16_t *frameBuf) : panel(panel), frameBuf(frameBuf) {}

Display::~Display()
{
    if (frameBuf != nullptr)
        heap_caps_free(frameBuf);
    if (presentBuf != nullptr)
        heap_caps_free(presentBuf);
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
    // X-mirror only: through the prism, text reads correctly this way. Enabling Y-mirror too
    // (esp_lcd_panel_mirror(panel_handle, true, true)) broke the image on this unit -- the
    // resulting upside-down scene is instead flipped in software, in presentFrame().
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    uint16_t *frame_buf =
        (uint16_t *)heap_caps_malloc(kDisplayWidth * kDisplayHeight * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (frame_buf == NULL)
    {
        ESP_LOGE(kDisplayTag, "failed to allocate frame buffer");
        abort();
    }
    // heap_caps_malloc() does not zero memory, and callers now fade toward black each frame
    // (see camera.h's fadeFrameBuffer()) rather than hard-clearing it -- without this, the
    // very first frame would fade whatever garbage happened to be in this memory instead of
    // starting from black, a brief noise flash at boot.
    std::memset(frame_buf, 0, kDisplayWidth * kDisplayHeight * sizeof(uint16_t));

    this->panel = panel_handle;
    this->frameBuf = frame_buf;
}

auto Display::getFrameBuf() -> uint16_t *
{
    return frameBuf;
}

void Display::presentFrame()
{
    if (presentBuf == nullptr)
    {
        presentBuf =
            (uint16_t *)heap_caps_malloc(kDisplayWidth * kDisplayHeight * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (presentBuf == NULL)
        {
            ESP_LOGE(kDisplayTag, "failed to allocate present buffer");
            abort();
        }
    }
    // Software Y-flip: copy frameBuf's rows into presentBuf in reverse order (row 0 <-> row
    // kDisplayHeight-1, etc). Safe to overwrite presentBuf here -- callers wait on the
    // previous frame's DMA (waitForFlushDone()) before rendering into frameBuf again, and
    // that wait always happens before this presentFrame() call in the same loop iteration.
    for (int y = 0; y < kDisplayHeight; ++y)
    {
        std::memcpy(presentBuf + y * kDisplayWidth, frameBuf + (kDisplayHeight - 1 - y) * kDisplayWidth,
                    kDisplayWidth * sizeof(uint16_t));
    }
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(this->panel, 0, 0, Display::kDisplayWidth, Display::kDisplayHeight, presentBuf));
}

auto Display::waitForFlushDone() -> bool
{
    return s_flushDone == nullptr || xSemaphoreTake(s_flushDone, portMAX_DELAY) == pdTRUE;
}

void Display::unpackColor565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t bField6 = (c >> 5) & 0x3F; // carries the physical BLUE value, see packColor565()
    uint8_t gField5 = c & 0x1F;        // carries the physical GREEN value, see packColor565()
    *r = uint8_t((r5 << 3) | (r5 >> 2));
    *b = uint8_t((bField6 << 2) | (bField6 >> 4));
    *g = uint8_t((gField5 << 3) | (gField5 >> 2));
}

uint16_t Display::blendColor565(uint16_t base, uint16_t target, uint16_t alphaQ8)
{
    uint8_t br, bg, bb, tr, tg, tb;
    unpackColor565(base, &br, &bg, &bb);
    unpackColor565(target, &tr, &tg, &tb);
    uint8_t r = uint8_t(int(br) + (((int(tr) - int(br)) * int(alphaQ8)) >> 8));
    uint8_t g = uint8_t(int(bg) + (((int(tg) - int(bg)) * int(alphaQ8)) >> 8));
    uint8_t b = uint8_t(int(bb) + (((int(tb) - int(bb)) * int(alphaQ8)) >> 8));
    return packColor565(r, g, b);
}

uint16_t Display::fadeColor565(uint16_t c, uint16_t keepQ8)
{
    uint8_t r, g, b;
    unpackColor565(c, &r, &g, &b);
    r = uint8_t((int(r) * int(keepQ8)) >> 8);
    g = uint8_t((int(g) * int(keepQ8)) >> 8);
    b = uint8_t((int(b) * int(keepQ8)) >> 8);
    return packColor565(r, g, b);
}