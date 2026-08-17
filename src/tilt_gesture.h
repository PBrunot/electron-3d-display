// Tilt-and-hold gesture detector, built on imu.h's QMI8658 accelerometer readings.
//
// Deliberately NOT a nudge/shake detector like micropython/nudge.py's (a short, sharp
// push detected via high-pass EMA filtering) -- this project's on-device interaction
// model was restarted (2026-08-17) to a sustained-tilt-with-confirm gesture instead:
// calibrate the resting "planar" orientation once at boot, then treat a large enough
// SUSTAINED deviation from that baseline in one dominant axis as a direction candidate.
// An arrow (drawTiltArrow() below) appears immediately so the user gets feedback that a
// direction was recognized; the action only fires once that same tilt has been HELD for
// kHoldConfirmMs, at which point poll() returns TiltPhase::kConfirmed exactly once (an
// edge, not a level) so callers can react to it with a plain if/switch instead of
// debouncing themselves. Releasing the tilt (dropping back near baseline) before the hold
// completes cancels the candidate with no action taken.
//
// Axis-to-screen-direction mapping is LEARNED on-device, not hardcoded: which physical
// accelerometer axis reads as "left" vs "up" depends on how the board sits in the pyramid
// rig, so guessing it (the way micropython/nudge.py's AXIS_SIGN_TO_DIRECTION table did,
// and an earlier version of this file also did) isn't reliable enough to trust blind.
// chooser.cpp's calibrateDirections() runs a guided sequence at boot (before the real
// menu appears) that asks the user to tilt-and-hold each of Right/Left/Up/Down in turn,
// using pollRaw() below (mapping-independent, like nudge.py's poll_raw()/poll() split) to
// read back which physical axis/sign each one actually is, then calls setMapping() to
// record it. Until calibrated, every axis/sign is unmapped (TiltDirection::kNone) and
// poll() reports no direction for any tilt -- a safe default, not a wrong-direction risk.
#pragma once

#include <cstdint>

#include "imu.h"
#include "orbitals.h" // orb_real_t

enum class TiltDirection : uint8_t
{
    kNone,
    kLeft,
    kRight,
    kUp,
    kDown,
};

/** Human-readable name, for logging only. */
const char *tiltDirectionName(TiltDirection dir);

enum class TiltPhase : uint8_t
{
    kIdle,     // no candidate direction currently held
    kHolding,  // a direction is held, still short of kHoldConfirmMs
    kConfirmed // hold just crossed kHoldConfirmMs -- fires exactly once per hold (an edge)
};

struct TiltEvent
{
    TiltDirection direction = TiltDirection::kNone;
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0; // elapsed hold time -- for a future progress animation on the arrow
};

/**
 * Like TiltEvent, but the raw physical axis (0/1/2 = x/y/z) and sign (+1/-1) instead of a
 * mapped TiltDirection -- axis is -1 when phase is kIdle (no candidate). Used by
 * calibrateDirections() to discover the mapping poll()/TiltEvent apply afterward.
 */
struct RawTiltEvent
{
    int axis = -1;
    int sign = 0;
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0;
};

struct TiltGestureConfig
{
    orb_real_t thresholdG = orb_real_t(0.28); // deviation magnitude to arm a candidate direction
    orb_real_t releaseG = orb_real_t(0.18);   // must drop below this to re-arm -- hysteresis so a
                                               // reading sitting right at thresholdG doesn't chatter
    uint32_t holdConfirmMs = 3000;
    int calibrationSamples = 40;
    uint32_t calibrationSampleDelayMs = 10;
};

class TiltGestureDetector
{
public:
    explicit TiltGestureDetector(Qmi8658 &imu, const TiltGestureConfig &cfg = TiltGestureConfig());

    /**
     * Average kCalibrationSamples accelerometer readings (kCalibrationSampleDelayMs apart)
     * into the "planar" baseline every later poll()/pollRaw() measures deviation against.
     * Call once at boot, board resting flat/still. Logs the captured baseline over serial.
     * Independent of direction calibration (see this file's header comment) -- this is
     * just the neutral-orientation reference, not the axis-to-direction mapping.
     */
    void calibrate();

    /** Record that physical axis/sign (as returned by pollRaw()) means `dir` on screen.
     * Called by chooser.cpp's calibrateDirections(); unmapped axis/signs stay kNone. */
    void setMapping(int axis, int sign, TiltDirection dir);

    /**
     * Read the IMU once and advance the hold state machine -- call once per frame/tick.
     * Mapping-independent (see this file's header comment): fires regardless of whether
     * setMapping() has been called for this axis/sign, so calibrateDirections() can use it
     * before any mapping exists.
     */
    RawTiltEvent pollRaw();

    /** Like pollRaw(), but translated through the learned mapping (setMapping()) into a
     * TiltDirection -- what every other caller in this project should use. An axis/sign
     * with no mapping yet reports TiltDirection::kNone (and never kConfirmed). */
    TiltEvent poll();

private:
    Qmi8658 &imu_;
    TiltGestureConfig cfg_;
    orb_real_t baseX_ = orb_real_t(0), baseY_ = orb_real_t(0), baseZ_ = orb_real_t(0);

    TiltDirection axisSignMap_[3][2] = {}; // [axis][sign>0 ? 1 : 0], default-initialized to kNone

    int activeAxis_ = -1;
    int activeSign_ = 0;
    uint32_t holdStartMs_ = 0;
    bool confirmedFired_ = false; // latched once kConfirmed has fired for this hold, so a
                                   // still-held gesture returns kHolding (not kConfirmed
                                   // again) on every subsequent poll()/pollRaw() until release
};

constexpr int kTiltArrowMarginPx = 14; // gap between the arrow's tip and the screen edge
constexpr int kTiltArrowLengthPx = 24; // tip-to-base along the pointing direction
constexpr int kTiltArrowHalfWidthPx = 14; // half the base width

/**
 * Draw a single filled triangle arrow at the center of the screen edge `dir` points
 * toward (kNone draws nothing). Plain/static for now -- no animation or text, see this
 * file's header comment; kTiltArrow*Px above are the only tuning knobs needed to add
 * either later without touching the rasterizer.
 */
void drawTiltArrow(uint16_t *frameBuf, TiltDirection dir, uint16_t color);
