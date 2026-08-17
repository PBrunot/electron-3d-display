#include "camera.h"

#include <cmath>

#include "esp_random.h"

void stepCamera(CameraState* cam) {
    cam->yaw += kCameraAngleStep;
    if (cam->yaw >= kTwoPi)
        cam->yaw -= kTwoPi;
    cam->tilt += kCameraTiltStep;
    if (cam->tilt >= kTwoPi)
        cam->tilt -= kTwoPi;
    cam->roll += kCameraRollStep;
    if (cam->roll >= kTwoPi)
        cam->roll -= kTwoPi;
}

RotationTrig computeRotationTrig(const CameraState& cam) {
    return RotationTrig{
        std::cos(cam.yaw), std::sin(cam.yaw), std::cos(cam.tilt),
        std::sin(cam.tilt), std::cos(cam.roll), std::sin(cam.roll),
    };
}

bool projectPoint(orb_real_t x, orb_real_t y, orb_real_t z, const RotationTrig& t, orb_real_t scale, int* outSx,
                   int* outSy) {
    orb_real_t rx1 = x * t.cosYaw + z * t.sinYaw;
    orb_real_t rz1 = z * t.cosYaw - x * t.sinYaw;
    orb_real_t ry2 = y * t.cosTilt - rz1 * t.sinTilt;
    orb_real_t rx3 = rx1 * t.cosRoll - ry2 * t.sinRoll;
    orb_real_t ry3 = rx1 * t.sinRoll + ry2 * t.cosRoll;

    int sx = Display::kDisplayWidth / 2 + int(std::lround(rx3 * scale));
    int sy = Display::kDisplayHeight / 2 - int(std::lround(ry3 * scale));
    if (sx < 0 || sx >= Display::kDisplayWidth || sy < 0 || sy >= Display::kDisplayHeight)
        return false;
    *outSx = sx;
    *outSy = sy;
    return true;
}

void drawProtonMarker(uint16_t* frameBuf, uint16_t color) {
    int x0 = Display::kDisplayWidth / 2 - kProtonMarkerSize / 2;
    int y0 = Display::kDisplayHeight / 2 - kProtonMarkerSize / 2;
    for (int y = y0; y < y0 + kProtonMarkerSize; y++)
        for (int x = x0; x < x0 + kProtonMarkerSize; x++)
            frameBuf[y * Display::kDisplayWidth + x] = color;
}

void fadeFrameBuffer(uint16_t* frameBuf) {
    int total = Display::kDisplayWidth * Display::kDisplayHeight;
    for (int i = 0; i < total; i++)
        frameBuf[i] = Display::fadeColor565(frameBuf[i], kPersistenceKeepQ8);
}

orb_real_t randomUnit() {
    return orb_real_t(esp_random()) / orb_real_t(4294967296.0); // 2^32
}

orb_real_t randomUniform(orb_real_t lo, orb_real_t hi) {
    return lo + (hi - lo) * randomUnit();
}

int nextZoomExcursionCountdown() {
    uint32_t span = uint32_t(kZoomExcursionMaxIntervalFrames - kZoomExcursionMinIntervalFrames + 1);
    return kZoomExcursionMinIntervalFrames + int(esp_random() % span);
}

int randomIndexExcluding(int current, int count) {
    // offset in [1, count-1] so current+offset (mod count) can never land back on current.
    int offset = 1 + int(esp_random() % uint32_t(count - 1));
    return (current + offset) % count;
}
