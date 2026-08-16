// ESP-IDF esp_lcd SPI driver + hydrogen-orbital point-cloud test for the
// Waveshare ESP32-S3-LCD-1.3 (ST7789V2, 240x240).
// Pinout and color-order/mirror correction per CLAUDE.md sections 2/3.
// Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/spi_lcd.html

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "atom_cloud.h"

#include <cmath>
#include <cstring>

// Uncomment to build a validation-data-dump instead of the interactive viewer: prints
// electron-configuration/Z_eff/point-sample data for a curated element set as tagged CSV
// log lines, for comparison against tools/orbitals_host/gen_atom_reference.py's host
// reference (run under the real MicroPython unix port -- see that script's docstring and
// tools/orbitals_host/compare_atom.py). Skips LCD init entirely; capture with e.g.
// `pio device monitor > capture.log`, then run compare_atom.py on it. See the plan this
// implements in the conversation that added it, and slater.h/atom_cloud.h for the ported
// model. Comment back out (and rebuild/reflash) to return to the interactive viewer.
// #define ATOM_VALIDATION_TEST

static const char *TAG = "orbital_test";

#define LCD_HOST     SPI2_HOST
#define PIN_MOSI     gpio_num_t(41)
#define PIN_SCLK     gpio_num_t(40)
#define PIN_CS       gpio_num_t(39)
#define PIN_DC       gpio_num_t(38)
#define PIN_RST      gpio_num_t(42)
#define PIN_BL       gpio_num_t(20)

#define LCD_WIDTH    240
#define LCD_HEIGHT   240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

// This panel's G and B sub-pixel drive lines are physically swapped (verified empirically:
// sending standard-RGB565 GREEN displays as blue and vice versa, while RED is unaffected -
// not explainable by MADCTL's BGR bit, which only ever swaps R/B and never touches G).
// No ST7789 register fixes a G/B swap, so values here are pre-swapped in software: the
// "green" field (bits 10:5) carries what should appear blue, and the "blue" field (bits 4:0)
// carries what should appear green. WHITE is unaffected (both fields full either way).
#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0xFFFF

// Full multi-electron atom point cloud (atom_cloud.h): angular tables are compile-time
// embedded (angular_library.h, confirmed working the same way kOrbital1sSampler was --
// xtensa-esp32s3-elf-nm showed it in .rodata, no runtime initializer call); each
// subshell's radial table is built at RUNTIME from slater.h's Z_eff model when this atom
// is picked (there are too many (n,ell,Z_eff) combinations across 118 elements to embed
// them all -- see pointcloud.h's angular/radial-split comment). Iron (Z=26) matches
// ATOMS.md's worked example (3d Z_eff=11.180). Switching elements is future UI work.
constexpr int kAtomicNumber = 26; // Fe

constexpr int kNumPoints = 2000;
constexpr uint32_t kRngSeed = 12345;
// Orbital-space-units-to-pixels scale. A full atom's outer/valence subshell reaches much
// further out (in Bohr radii) than a bare 1s cloud does, so this is a rough starting
// guess, NOT calibrated the way ATOMS.md's pixels_per_bohr_for_canvas() is on the PC
// side -- almost certainly needs visual tuning once actually seen on the device.
constexpr orb_real_t kScale = orb_real_t(10);
constexpr orb_real_t kAnglePerFrame = orb_real_t(0.05);

#ifdef ATOM_VALIDATION_TEST
// MUST match tools/orbitals_host/gen_atom_reference.py's ATOM_TEST_CASES/SEED/POINTS_PER_CASE
// exactly, or the two sides aren't comparing the same thing.
constexpr int kValidationZs[] = {1, 2, 6, 10, 24, 26, 46, 58}; // H, He, C, Ne, Cr, Fe, Pd, Ce
constexpr uint32_t kValidationSeed = 12345;
constexpr int kValidationPoints = 50;

static void runAtomValidationTest() {
    static AtomPoint points[kValidationPoints];

    for (int zi = 0; zi < int(sizeof(kValidationZs) / sizeof(kValidationZs[0])); zi++) {
        int z = kValidationZs[zi];
        const char *symbol = elementSymbol(z);
        ElectronConfig config = buildAtomPointCloud(z, points, kValidationPoints, kValidationSeed);

        for (int i = 0; i < config.count; i++) {
            ESP_LOGI(TAG, "ATOMTEST,CONFIG,%s,%d,%d,%d", symbol, config.subshells[i].n, config.subshells[i].ell,
                     config.subshells[i].occ);
        }
        for (int i = 0; i < config.count; i++) {
            int n = config.subshells[i].n, ell = config.subshells[i].ell;
            orb_real_t zEff = zEffRadial(z, config, n, ell);
            ESP_LOGI(TAG, "ATOMTEST,ZEFF,%s,%d,%d,%.17g", symbol, n, ell, double(zEff));
        }
        for (int i = 0; i < kValidationPoints; i++) {
            ESP_LOGI(TAG, "ATOMTEST,POINT,%s,%d,%.17g,%.17g,%.17g", symbol, i, double(points[i].x),
                     double(points[i].y), double(points[i].z));
        }
    }
    ESP_LOGI(TAG, "ATOMTEST,DONE");
}
#endif

extern "C" void app_main(void) {
#ifdef ATOM_VALIDATION_TEST
    while (1) {
        runAtomValidationTest();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif

    gpio_config_t bl_cfg = {};
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    bl_cfg.pin_bit_mask = 1ULL << PIN_BL;
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level((gpio_num_t)PIN_BL, 1);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_SCLK;
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
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

    // Sample the point cloud ONCE (builds every occupied subshell's radial table as it
    // goes); only the rotation/projection below runs every frame (matches CLAUDE.md §5:
    // generate points once per orbital/atom, then just rotate/reproject -- not per-frame
    // resampling).
    static AtomPoint points[kNumPoints];
    int64_t buildStartUs = esp_timer_get_time();
    ElectronConfig config = buildAtomPointCloud(kAtomicNumber, points, kNumPoints, kRngSeed);
    int64_t buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

    ESP_LOGI(TAG, "%s (Z=%d): %d subshells, %d points, built in %lldms", elementSymbol(kAtomicNumber),
             kAtomicNumber, config.count, kNumPoints, buildMs);
    for (int i = 0; i < config.count; i++) {
        ESP_LOGI(TAG, "  %d%c%d", config.subshells[i].n, subshellLabelChar(config.subshells[i].ell),
                 config.subshells[i].occ);
    }

    uint16_t *frame_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (frame_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate frame buffer");
        return;
    }

    // FPS benchmark: no fixed per-frame delay (a fixed vTaskDelay(33) would just measure
    // the delay, not the hardware's real ceiling) -- vTaskDelay(1) below is the minimum
    // yield to keep FreeRTOS's idle/watchdog task fed, ~10ms/tick at the default 100Hz
    // tick rate, i.e. a ~100 FPS ceiling from the delay alone -- well above the ~40 FPS
    // theoretical ceiling from the SPI transfer itself (CLAUDE.md §6: 240x240x16bit at
    // 40MHz ~= 23ms/frame), so the delay should never be the bottleneck we're measuring.
    constexpr int kFpsReportEveryNFrames = 60;
    int64_t reportWindowStartUs = esp_timer_get_time();
    int framesSinceReport = 0;

    orb_real_t angle = 0;
    while (1) {
        orb_real_t cosA = std::cos(angle);
        orb_real_t sinA = std::sin(angle);

        std::memset(frame_buf, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
        for (int i = 0; i < kNumPoints; i++) {
            // Rotate around the polar (z) axis, in the x/y plane; z (screen-vertical below)
            // is the rotation axis itself so it's untouched by this rotation.
            orb_real_t x = points[i].x * cosA + points[i].y * sinA;

            int sx = int(orb_real_t(LCD_WIDTH) / 2 + x * kScale);
            int sy = int(orb_real_t(LCD_HEIGHT) / 2 + points[i].z * kScale);
            if (sx < 0 || sx >= LCD_WIDTH || sy < 0 || sy >= LCD_HEIGHT)
                continue;
            frame_buf[sy * LCD_WIDTH + sx] = COLOR_WHITE;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, frame_buf));

        angle += kAnglePerFrame;

        framesSinceReport++;
        if (framesSinceReport >= kFpsReportEveryNFrames) {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - reportWindowStartUs) / 1e6;
            double fps = double(framesSinceReport) / elapsedS;
            ESP_LOGI(TAG, "FPS: %.1f (%d frames / %.3fs, %d points/frame)", fps, framesSinceReport, elapsedS,
                     kNumPoints);
            reportWindowStartUs = nowUs;
            framesSinceReport = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
