#include "debug/screenshot_console.h"

#include <cstdio>
#include <cstring>

#include "util/crc32.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "debug/screenshot.h"
#include "debug/screenshot_batch.h"
#include "debug/screenshot_pause.h"

namespace
{
    Display *g_display = nullptr;

    /// Scratch for one flat, row-major snapshot of the frame buffer (see Display::
    /// readAllPixels()) -- allocated once (startScreenshotConsole()) from the regular heap,
    /// not a static reservation: this is a debug feature exercised rarely, and on a
    /// no-PSRAM board (CYD) a static kDisplayWidth*kDisplayHeight array would otherwise
    /// permanently reserve real budget from an already tight internal-RAM link.
    uint16_t *g_pixelBuf = nullptr;

    // Generous: the active render loop only reaches a checkpoint() between frames/bounded
    // transitions (see screenshot_pause.h) -- a boot fly-over or preset-switch transition can
    // take a couple of seconds, so this needs real margin above that, not just one frame.
    constexpr uint32_t kPauseTimeoutMs = 5000;

    constexpr char kB64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// Encodes `len` (<= kChunkRawBytes) raw bytes into `out` (must hold at least
    /// 4*((len+2)/3)+1 bytes, i.e. kB64LineCapacity for len == kChunkRawBytes).
    void base64EncodeChunk(const uint8_t *data, size_t len, char *out)
    {
        size_t o = 0, i = 0;
        for (; i + 3 <= len; i += 3)
        {
            uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
            out[o++] = kB64Alphabet[(n >> 18) & 0x3F];
            out[o++] = kB64Alphabet[(n >> 12) & 0x3F];
            out[o++] = kB64Alphabet[(n >> 6) & 0x3F];
            out[o++] = kB64Alphabet[n & 0x3F];
        }
        size_t rem = len - i;
        if (rem == 1)
        {
            uint32_t n = uint32_t(data[i]) << 16;
            out[o++] = kB64Alphabet[(n >> 18) & 0x3F];
            out[o++] = kB64Alphabet[(n >> 12) & 0x3F];
            out[o++] = '=';
            out[o++] = '=';
        }
        else if (rem == 2)
        {
            uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
            out[o++] = kB64Alphabet[(n >> 18) & 0x3F];
            out[o++] = kB64Alphabet[(n >> 12) & 0x3F];
            out[o++] = kB64Alphabet[(n >> 6) & 0x3F];
            out[o++] = '=';
        }
        out[o] = 0;
    }

    // 48 -> multiple of 3 (no internal padding except possibly the file's final chunk) that
    // keeps each printed line short (64 base64 chars).
    constexpr size_t kChunkRawBytes = 48;
    constexpr size_t kB64LineCapacity = 4 * ((kChunkRawBytes + 2) / 3) + 1;

    void printFileVisitor(const char *name, size_t size, void *)
    {
        printf("SS_FILE %s %u\n", name, (unsigned)size);
    }

    void handleCap()
    {
        if (g_display == nullptr || g_pixelBuf == nullptr)
        {
            printf("SS_ERR no display\n");
            return;
        }
        if (!screenshot_pause::requestPause(kPauseTimeoutMs))
        {
            printf("SS_ERR pause timeout\n");
            return;
        }
        // The paused render loop may have queued its last presentFrame() DMA transfer
        // without yet reaching its own waitForFlushDone() (that only happens at the top of
        // its NEXT iteration, after this checkpoint) -- some blocks' DMA may still be in
        // flight. syncForExternalRead() is safe to call here specifically because the render
        // loop task is guaranteed quiesced by the pause above, so nothing else can be waiting
        // on the same completion signal right now.
        g_display->syncForExternalRead();
        g_display->readAllPixels(g_pixelBuf);
        char name[screenshot::kMaxNameLen];
        size_t size = 0;
        bool ok = screenshot::capture(g_pixelBuf, name, sizeof(name), &size);
        screenshot_pause::releasePause();

        if (ok)
            printf("SS_CAPTURED %s %u\n", name, (unsigned)size);
        else
            printf("SS_ERR capture failed\n");
    }

    void handleList()
    {
        screenshot::forEachFile(printFileVisitor, nullptr);
        printf("SS_LIST_END\n");
    }

    void handleGet(const char *name)
    {
        size_t size = 0;
        uint8_t *data = screenshot::readFile(name, &size);
        if (data == nullptr)
        {
            printf("SS_ERR not found\n");
            return;
        }
        uint32_t crc = crc32(data, size);
        printf("SS_BEGIN %s %u %08x\n", name, (unsigned)size, (unsigned)crc);

        char line[kB64LineCapacity];
        for (size_t off = 0; off < size; off += kChunkRawBytes)
        {
            size_t n = size - off < kChunkRawBytes ? size - off : kChunkRawBytes;
            base64EncodeChunk(data + off, n, line);
            printf("SS_DATA %s\n", line);
        }
        printf("SS_END\n");
        heap_caps_free(data);
    }

    void handleCapAll()
    {
        printf("SS_CAP_ALL_START\n");
        // Batch capture renders into its own offscreen Display (never touches the live one),
        // but OrbitalPresetState::load()/AtomPresetState::load() still write through shared
        // static scratch (see screenshot_pause.h) -- pausing is required even though this
        // command never reads the live display's pixels.
        if (!screenshot_pause::requestPause(kPauseTimeoutMs))
        {
            printf("SS_ERR pause timeout\n");
            return;
        }
        captureAllPresets(); // blocks (tens of seconds); progress is logged via ESP_LOGI, not this protocol
        screenshot_pause::releasePause();
        printf("SS_CAP_ALL_DONE\n");
    }

    void handleDel(const char *name)
    {
        if (screenshot::deleteFile(name))
            printf("SS_DELETED %s\n", name);
        else
            printf("SS_ERR delete failed\n");
    }

    void consoleTask(void *)
    {
        // Standard ESP-IDF console boilerplate: without uart_vfs_dev_use_driver(), stdin
        // falls back to a busy-polling ROM UART read that doesn't play well with a blocking
        // fgets() in a background task -- this switches UART0 to the interrupt-driven driver
        // + VFS so fgets() actually blocks/yields instead of spinning the CPU. (IDF 6.0.1
        // renamed these from the older esp_vfs_dev_uart_* names -- see
        // components/esp_driver_uart/include/driver/uart_vfs.h.)
        uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
        uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
        uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0);
        uart_vfs_dev_use_driver(UART_NUM_0);

        char line[64];
        while (true)
        {
            if (fgets(line, sizeof(line), stdin) == nullptr)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            size_t len = std::strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = 0;
            if (len == 0)
                continue;

            if ((len == 1 && line[0] == 's') || std::strcmp(line, "SS_CAP") == 0)
                handleCap();
            else if ((len == 1 && line[0] == 'l') || std::strcmp(line, "SS_LIST") == 0)
                handleList();
            else if ((len == 1 && line[0] == 'a') || std::strcmp(line, "SS_CAP_ALL") == 0)
                handleCapAll();
            else if (std::strncmp(line, "SS_GET ", 7) == 0)
                handleGet(line + 7);
            else if (std::strncmp(line, "SS_DEL ", 7) == 0)
                handleDel(line + 7);
            else
                printf("SS_ERR unknown command\n");
        }
    }
} // namespace

void startScreenshotConsole(Display &display)
{
    g_display = &display;
    g_pixelBuf = (uint16_t *)heap_caps_malloc(
        size_t(Display::kDisplayWidth) * Display::kDisplayHeight * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (g_pixelBuf == nullptr)
        printf("SS_ERR failed to allocate screenshot scratch buffer -- capture disabled\n");
    screenshot::init();
    screenshot_pause::init();
    // 8192, matching CONFIG_ESP_MAIN_TASK_STACK_SIZE (sdkconfig.defaults): since the
    // 'a'/SS_CAP_ALL batch command (screenshot_batch.cpp) runs the exact same point-cloud-
    // building call chain (OrbitalPresetState::load()/AtomPresetState::load()) as the main
    // task's chooser/orbital_view/atom_view loop, this task deserves the same headroom --
    // the original 4096 was sized only for the lightweight single-shot capture/list/base64
    // commands.
    xTaskCreate(consoleTask, "ss_console", 8192, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}
