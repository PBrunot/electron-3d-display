#include "debug/screenshot_pause.h"

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace screenshot_pause
{
namespace
{
    std::atomic<bool> gPauseRequested{false};
    SemaphoreHandle_t gPausedAck = nullptr; // given by a render loop when it parks in checkpoint()
    SemaphoreHandle_t gResume = nullptr;    // given by releasePause() to unpark it
} // namespace

void init()
{
    gPausedAck = xSemaphoreCreateBinary();
    gResume = xSemaphoreCreateBinary();
}

void checkpoint()
{
    if (!gPauseRequested.load(std::memory_order_acquire))
        return;
    xSemaphoreGive(gPausedAck);
    xSemaphoreTake(gResume, portMAX_DELAY);
}

bool requestPause(uint32_t timeoutMs)
{
    gPauseRequested.store(true, std::memory_order_release);
    bool ok = xSemaphoreTake(gPausedAck, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    if (!ok)
        gPauseRequested.store(false, std::memory_order_release); // give up -- let it keep running
    return ok;
}

void releasePause()
{
    // Clear the flag BEFORE giving gResume: the paused loop wakes as soon as gResume is
    // given and immediately loops back around to its next checkpoint() call, which must see
    // the flag already false -- otherwise it re-parks instantly and this pause never
    // actually ends.
    gPauseRequested.store(false, std::memory_order_release);
    xSemaphoreGive(gResume);
}

} // namespace screenshot_pause
