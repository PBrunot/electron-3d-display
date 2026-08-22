#include "render/display.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32
// CYD (ESP32-2432S028R). Pins verified against witnessmenow/ESP32-Cheap-Yellow-Display
// (PINS.md + Examples/Basics/1-HelloWorld/platformio.ini), NOT yet against physical
// hardware for this project -- re-verify with the examples/corner_calibration methodology
// (see CLAUDE.md) before trusting mirror/color settings below on a real unit.
#include "esp_lcd_ili9341.h"

#define LCD_HOST SPI2_HOST // SPI2_HOST == HSPI on classic ESP32, matches PINS.md's "Uses HSPI"
#define PIN_MOSI gpio_num_t(13)
#define PIN_SCLK gpio_num_t(14)
#define PIN_CS gpio_num_t(15)
#define PIN_DC gpio_num_t(2)
#define PIN_RST gpio_num_t(-1) // display RESET ties to board RESET on the CYD, no GPIO for it
#define PIN_BL gpio_num_t(21)

// The CYD example repo's own verified-working SPI_FREQUENCY for this exact ILI9341 wiring
// is 55MHz; running at a more conservative 40MHz here (TFT_eSPI's User_Setup.h notes "with
// an ILI9341 display 40MHz works OK" too) -- raise back toward 55MHz once color/mirror is
// confirmed correct on real hardware, if higher FPS is needed.
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#else
#define LCD_HOST SPI2_HOST
#define PIN_MOSI gpio_num_t(41)
#define PIN_SCLK gpio_num_t(40)
#define PIN_CS gpio_num_t(39)
#define PIN_DC gpio_num_t(38)
#define PIN_RST gpio_num_t(42)
#define PIN_BL gpio_num_t(20)

/**
 * SPI pixel clock. The GP-SPI clock divides a fixed 80MHz peripheral clock by an integer
 * only (80, 40, 26.7, 20 MHz, ...); a non-achievable request silently snaps DOWN to the
 * nearest achievable rate with no warning logged, so only request achievable values. 80MHz
 * is the fastest achievable step and exceeds the ST7789VW datasheet's Table 6 Tscycw spec
 * (16ns min write cycle = 62.5MHz max) by 28% -- out of spec; watch for pixel glitches/noise
 * if the panel or wiring changes.
 */
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#endif

auto Display::onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx) -> bool
{
    Display *self = static_cast<Display *>(userCtx);
    BaseType_t highPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(self->s_flushDone, &highPriorityTaskWoken);
    return highPriorityTaskWoken == pdTRUE;
}

Display::Display(esp_lcd_panel_handle_t panel, uint16_t *frameBuf) : panel(panel)
{
    // Test seam: the whole logical buffer as a single block, matching the shape the real
    // constructor produces when the heap happens to satisfy it in one allocation.
    blocks[0] = frameBuf;
    blockCount = 1;
    rowsPerBlock = kDisplayHeight;
}

Display::~Display()
{
    for (int i = 0; i < blockCount; i++)
        if (blocks[i] != nullptr)
            heap_caps_free(blocks[i]);
    if (panel != nullptr)
        ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
}

Display::Display()
{
    // Counting, not binary -- presentFrame() queues every block's DMA transfer without waiting
    // between them, so up to kMaxBlocks completions can be outstanding before
    // waitForFlushDone() drains them (see display.h). Starts at count 0 / pendingFlushCount 0:
    // no frame is in flight yet, so the first waitForFlushDone() (before the first
    // presentFrame()) must not block.
    this->s_flushDone = xSemaphoreCreateCounting(kMaxBlocks, 0);

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

    // Log the SPI clock actually achievable, not just the requested LCD_PIXEL_CLOCK_HZ: the
    // GP-SPI clock divider only hits discrete steps (80/N MHz), so a requested rate silently
    // snaps down with no warning from the driver (see LCD_PIXEL_CLOCK_HZ comment above).
    // esp_lcd_panel_io_spi keeps its spi_device_handle_t private, so there's no public way to
    // query the real device's actual frequency directly -- instead, add a throwaway probe
    // device on the same bus with identical clock_speed_hz/mode/half-duplex flag (the inputs
    // that feed the HAL's clock divisor calculation) so spi_device_get_actual_freq() reports
    // the same value the real LCD device resolves to, then remove it immediately.
    {
        spi_device_interface_config_t probe_cfg = {};
        probe_cfg.mode = 0;
        probe_cfg.clock_speed_hz = LCD_PIXEL_CLOCK_HZ;
        probe_cfg.spics_io_num = -1;
        probe_cfg.queue_size = 1;
        probe_cfg.flags = SPI_DEVICE_HALFDUPLEX;
        spi_device_handle_t probe_dev = NULL;
        if (spi_bus_add_device(LCD_HOST, &probe_cfg, &probe_dev) == ESP_OK)
        {
            int actualKhz = 0;
            spi_device_get_actual_freq(probe_dev, &actualKhz);
            ESP_LOGI(kDisplayTag, "SPI bus: requested %.2f MHz, actual %.3f MHz",
                     LCD_PIXEL_CLOCK_HZ / 1e6, actualKhz / 1000.0);
            spi_bus_remove_device(probe_dev);
        }
        else
        {
            ESP_LOGW(kDisplayTag, "SPI bus: could not probe actual clock (requested %.2f MHz)",
                     LCD_PIXEL_CLOCK_HZ / 1e6);
        }
    }

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
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
#if CONFIG_IDF_TARGET_ESP32
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
#else
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
#endif

    // Allocate the frame buffer as several independent DMA-capable blocks instead of one
    // heap_caps_malloc() covering kDisplayWidth*kDisplayHeight: internal SRAM on the CYD is
    // fragmented into several non-contiguous heap regions (CYD-branch.md), so a single large
    // request can fail even when the SUM of free regions would be enough. Query the largest
    // contiguous free block, size every block to fit comfortably inside it (leaving headroom
    // for other allocations rather than grabbing every last free byte), then allocate blocks of
    // that size until the whole buffer is covered. If reality turns out more fragmented than the
    // query suggested (a later block fails), back off by halving the block size and retry from
    // scratch -- bounded by rowsPerBlock reaching 1, at which point a real allocation failure is
    // reported and the app aborts (same failure-handling convention as the rest of this file).
    // On the S3 this almost always resolves to a single block on the first try.
    {
        constexpr size_t kRowBytes = size_t(kDisplayWidth) * sizeof(uint16_t);
        constexpr size_t kSafetyMarginBytes = 8 * 1024;

        size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        // Total, not just largest-block: on a fragmented heap (CYD) the largest single block
        // understates what block-splitting can actually reach, and logging both is what let
        // CYD-branch.md's RAM budget be tuned against real hardware instead of guesswork --
        // keep this line, it's the fastest way to re-check headroom after any future change to
        // kOrbitalNumPoints/kAtomNumPoints or Display::kDisplayWidth/Height.
        size_t totalDmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t budget = largestFree > kSafetyMarginBytes ? largestFree - kSafetyMarginBytes : 0;
        int candidateRows = int(budget / kRowBytes);
        if (candidateRows > kDisplayHeight)
            candidateRows = kDisplayHeight;
        if (candidateRows < 1)
            candidateRows = 1;
        ESP_LOGI(kDisplayTag,
                 "frame buffer: %dx%d logical (%u bytes needed), largest free DMA block=%u bytes, "
                 "total free DMA=%u bytes -> starting at %d rows/block",
                 kDisplayWidth, kDisplayHeight, unsigned(size_t(kDisplayWidth) * kDisplayHeight * sizeof(uint16_t)),
                 unsigned(largestFree), unsigned(totalDmaFree), candidateRows);

        for (;;)
        {
            for (int i = 0; i < blockCount; i++)
                heap_caps_free(blocks[i]);
            blockCount = 0;
            rowsPerBlock = candidateRows;

            int need = (kDisplayHeight + rowsPerBlock - 1) / rowsPerBlock;
            bool ok = true;
            for (int i = 0; i < need; i++)
            {
                int rows = std::min(rowsPerBlock, kDisplayHeight - i * rowsPerBlock);
                uint16_t *blk = (uint16_t *)heap_caps_malloc(size_t(rows) * kRowBytes, MALLOC_CAP_DMA);
                if (blk == nullptr)
                {
                    ok = false;
                    break;
                }
                blocks[blockCount++] = blk;
            }
            if (ok)
                break;
            if (candidateRows == 1)
            {
                ESP_LOGE(kDisplayTag, "failed to allocate frame buffer (even at 1 row/block)");
                abort();
            }
            int nextRows = std::max(1, candidateRows / 2);
            ESP_LOGW(kDisplayTag, "block alloc failed at %d rows/block after %d block(s) -- retrying at %d",
                     candidateRows, blockCount, nextRows);
            candidateRows = nextRows;
        }
        int lastBlockRows = kDisplayHeight - (blockCount - 1) * rowsPerBlock;
        ESP_LOGI(kDisplayTag,
                 "frame buffer: allocated %d block(s), %d rows each (%u bytes) + last block %d rows (%u bytes), "
                 "%u bytes total",
                 blockCount, rowsPerBlock, unsigned(size_t(rowsPerBlock) * kRowBytes), lastBlockRows,
                 unsigned(size_t(lastBlockRows) * kRowBytes),
                 unsigned(size_t(kDisplayWidth) * kDisplayHeight * sizeof(uint16_t)));

        // heap_caps_malloc() does not zero memory, and callers now fade toward black each
        // frame (see Display::fade(), used by camera.h's renderScene()/renderSceneGrouped())
        // rather than hard-clearing it -- without this, the very first frame would fade
        // whatever garbage happened to be in this memory instead of starting from black, a
        // brief noise flash at boot.
        for (int i = 0; i < blockCount; i++)
        {
            int rows = std::min(rowsPerBlock, kDisplayHeight - i * rowsPerBlock);
            std::memset(blocks[i], 0, size_t(rows) * kRowBytes);
        }
    }

    // No letterbox on either target: the logical canvas above is always the full physical
    // panel now, so there's no border area for presentFrame() to leave untouched -- the very
    // first presentFrame() (main.cpp, after drawSplashScreen()) covers the whole GRAM itself.
    this->panel = panel_handle;
}

void Display::clearScreen()
{
    // Direct per-block scan, not a writePx() loop -- doesn't need physicalRow() (every pixel
    // is touched regardless of logical row identity), just storageColor() once for the fill
    // value, so this stays as cheap as the old single-array version.
    uint16_t stored = storageColor(kColorBlack);
    for (int i = 0; i < blockCount; i++)
    {
        int rows = std::min(rowsPerBlock, kDisplayHeight - i * rowsPerBlock);
        int count = rows * kDisplayWidth;
        std::fill(blocks[i], blocks[i] + count, stored);
    }
}

void Display::fade(uint16_t keepQ8)
{
    // fadeColor565() operates on standard-layout RGB565 (see storageColor()'s docstring), so
    // each stored pixel is decoded before the fade math and re-encoded after -- a no-op pair
    // on the S3 (storageColor() is the identity there), an actual byte-swap pair on the CYD.
    for (int i = 0; i < blockCount; i++)
    {
        int rows = std::min(rowsPerBlock, kDisplayHeight - i * rowsPerBlock);
        int count = rows * kDisplayWidth;
        uint16_t *blk = blocks[i];
        for (int j = 0; j < count; j++)
            blk[j] = storageColor(fadeColor565(storageColor(blk[j]), keepQ8));
    }
}

void Display::blit(int x, int y, const uint16_t *src, int srcWidth, int srcHeight)
{
    // `src` is always standard-layout RGB565 (every generated bitmap in this project is
    // packed via packColor565(), see e.g. splash_bitmap.h) -- on the S3 storageColor() is the
    // identity, so a straight memcpy is correct and fastest; on the CYD every pixel needs an
    // actual byte-swap on the way in, so that path can't just memcpy the source bytes as-is.
    for (int row = 0; row < srcHeight; row++)
    {
        int py = y + row;
        if (py < 0 || py >= kDisplayHeight)
            continue;
        int colStart = x < 0 ? -x : 0;
        int colEnd = std::min(srcWidth, kDisplayWidth - x);
        if (colStart >= colEnd)
            continue;
        int physY = physicalRow(py);
        uint16_t *blk = blocks[physY / rowsPerBlock];
        int rowInBlock = physY % rowsPerBlock;
        uint16_t *dst = blk + size_t(rowInBlock) * kDisplayWidth + (x + colStart);
        const uint16_t *s = src + row * srcWidth + colStart;
        int n = colEnd - colStart;
#if CONFIG_IDF_TARGET_ESP32
        for (int i = 0; i < n; i++)
            dst[i] = storageColor(s[i]);
#else
        std::memcpy(dst, s, size_t(n) * sizeof(uint16_t));
#endif
    }
}

void Display::readAllPixels(uint16_t *dest) const
{
    // Plain readPx() loop -- this is only used by screenshot capture (a rare, non-hot path),
    // so simplicity wins over the per-block memcpy fast paths used by clearScreen()/blit().
    for (int y = 0; y < kDisplayHeight; y++)
        for (int x = 0; x < kDisplayWidth; x++)
            dest[y * kDisplayWidth + x] = readPx(x, y);
}

void Display::presentFrame()
{
    // Every block already holds pixels in the exact byte order and physical row order the
    // panel needs (see physicalRow()/storageColor() in display.h) -- so sending a frame is one
    // direct DMA transfer per block, no scratch buffer and no per-pixel transform here at all,
    // fully unified between targets.
    //
    // Blocks are queued back-to-back with no wait in between: fire-and-forget, not
    // synchronous. esp_lcd_panel_draw_bitmap() queues onto the SPI driver's own transaction
    // queue (io_config.trans_queue_depth = 10) and only blocks the caller if that queue is
    // full, so this naturally throttles instead of overflowing on a heavily fragmented heap
    // with more than 10 blocks -- it just stops returning early in that rare case. Each
    // block's completion (via onColorTransDone(), the ISR callback) gives s_flushDone once;
    // waitForFlushDone() is what actually waits them out, on whichever call touches the frame
    // buffer next.
    // Logical canvas == physical panel on both targets now (no letterbox).
    constexpr int kOffsetX = 0;
    constexpr int kOffsetY = 0;
    for (int b = 0; b < blockCount; b++)
    {
        int blockRows = std::min(rowsPerBlock, kDisplayHeight - b * rowsPerBlock);
        int screenY = kOffsetY + b * rowsPerBlock;
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(this->panel, kOffsetX, screenY, kOffsetX + kDisplayWidth,
                                                   screenY + blockRows, blocks[b]));
    }
    this->pendingFlushCount = blockCount;
}

auto Display::waitForFlushDone() -> bool
{
    if (s_flushDone == nullptr)
        return true;
    while (pendingFlushCount > 0)
    {
        if (xSemaphoreTake(s_flushDone, portMAX_DELAY) != pdTRUE)
            return false;
        pendingFlushCount--;
    }
    return true;
}

auto Display::syncForExternalRead() -> bool
{
    bool ok = waitForFlushDone();
    if (s_flushDone != nullptr)
    {
        // Give back exactly one completion so the render loop's own next waitForFlushDone()
        // call (once unpaused) finds "ready" immediately instead of blocking on a signal this
        // call already drained -- see this method's docstring in display.h.
        xSemaphoreGive(s_flushDone);
        pendingFlushCount++;
    }
    return ok;
}

void Display::unpackColor565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5) & 0x3F;
    uint8_t b5 = c & 0x1F;
    *r = uint8_t((r5 << 3) | (r5 >> 2));
    *g = uint8_t((g6 << 2) | (g6 >> 4));
    *b = uint8_t((b5 << 3) | (b5 >> 2));
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
