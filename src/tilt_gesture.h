// Tilt-and-hold gesture detector, built on imu.h's QMI8658 accelerometer readings.
//
// Deliberately NOT a nudge/shake detector like micropython/nudge.py's (a short, sharp
// push detected via high-pass EMA filtering) -- this project's on-device interaction
// model was restarted (2026-08-17) to a sustained-tilt-with-confirm gesture instead:
// calibrate the resting "planar" orientation once at boot, then treat a large enough
// SUSTAINED deviation from that baseline as a direction candidate. An arrow
// (drawTiltArrow() below) appears immediately so the user gets feedback that a direction
// was recognized; the action only fires once that same tilt has been HELD for
// kHoldConfirmMs, at which point poll() returns TiltPhase::kConfirmed exactly once (an
// edge, not a level) so callers can react to it with a plain if/switch instead of
// debouncing themselves. Releasing the tilt (dropping back near baseline) before the hold
// completes cancels the candidate with no action taken.
//
// Direction matching is FULL-3D-VECTOR cosine similarity, not "pick the single axis with
// the largest raw deviation" (what an earlier version of this file did, and what
// micropython/nudge.py's AXIS_SIGN_TO_DIRECTION table assumed too): on real hardware that
// dominant-axis heuristic proved unreliable ("maybe the planar is wrong", 2026-08-17
// feedback) -- it implicitly assumes the board's rest ("planar") orientation lines up
// cleanly with a sensor axis, but the pyramid rig's actual mount angle isn't guaranteed
// to. Comparing the FULL normalized deviation vector against each calibrated reference
// direction via dot product (the standard nearest-direction/cosine-similarity classifier,
// see e.g. NXP AN3461/ST DT0140's accelerometer-tilt application notes) works regardless
// of how gravity happens to project onto the sensor's own X/Y/Z axes at rest, so it
// doesn't carry that assumption. Direction mapping is still LEARNED on-device, not
// hardcoded: chooser.cpp's calibrateDirections() runs a guided sequence at boot (before
// the real menu appears) that asks the user to tilt-and-hold each of Right/Left/Up/Down in
// turn, using pollRaw() below (mapping-independent, like nudge.py's poll_raw()/poll()
// split) to capture each one's actual deviation vector, then calls setMapping() to record
// it as that direction's reference. Until calibrated, every direction has no reference
// vector and poll() reports no direction for any tilt -- a safe default, not a
// wrong-direction risk.
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
constexpr int kTiltDirectionCount = 4; // kLeft..kDown, i.e. excluding kNone

/** Human-readable name, for logging only. */
const char *tiltDirectionName(TiltDirection dir);

enum class TiltPhase : uint8_t
{
    kIdle,        // no candidate direction currently held
    kHolding,     // a direction is held, still short of kHoldConfirmMs
    kConfirmed,   // hold just crossed kHoldConfirmMs -- fires exactly once per hold (an edge)
    kConfirmedLong // hold just ALSO crossed cfg.holdConfirmLongMs -- fires exactly once per
                   // hold, strictly after kConfirmed already fired for the same hold. Lets a
                   // single direction carry two actions (short hold vs. long hold) instead
                   // of needing a 5th/6th gesture direction -- see AtomView's Up/Down, which
                   // use short-hold for periodic-table movement and long-hold for
                   // menu-return/dissection (the actions that direction used to fire on
                   // plain kConfirmed, before periodic-table navigation took it over).
};

struct TiltEvent
{
    TiltDirection direction = TiltDirection::kNone;
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0; // elapsed hold time -- for a future progress animation on the arrow
};

/**
 * Like TiltEvent, but the raw normalized deviation-from-baseline direction (dirX/dirY/
 * dirZ, a unit vector in board-local accelerometer axes) instead of a mapped
 * TiltDirection -- valid only while phase != kIdle. Used by calibrateDirections() to
 * capture each target direction's reference vector for poll()/TiltEvent to match against
 * afterward (see this file's header comment on why a full vector, not an axis index).
 */
struct RawTiltEvent
{
    orb_real_t dirX = orb_real_t(0), dirY = orb_real_t(0), dirZ = orb_real_t(0);
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0;
};

struct TiltGestureConfig
{
    orb_real_t thresholdG = orb_real_t(0.28); // deviation magnitude to arm a candidate direction
    orb_real_t releaseG = orb_real_t(0.18);   // must drop below this to re-arm -- hysteresis so a
                                               // reading sitting right at thresholdG doesn't chatter
    uint32_t holdConfirmMs = 1000; // "3s is a lot to switch" (feedback, 2026-08-17) -- was 3000
    // Extra hold time PAST holdConfirmMs before kConfirmedLong fires -- see TiltPhase's
    // comment. Not "3s total" (the threshold this project already moved away from above):
    // the short action already fired at holdConfirmMs, so this is purely the extra
    // deliberate-hold time to reach the second, different action on the same direction.
    uint32_t holdConfirmLongMs = 1500;

    // Baseline ("planar") calibration -- longer and slower than an earlier version of this
    // file used (40 samples/400ms total), per calibration write-ups for this same class of
    // sensor (e.g. MPU6050 calibration guides) recommending averaging over roughly a
    // second rather than a few hundred ms for a stable zero-tilt reference.
    int calibrationSamples = 100;
    uint32_t calibrationSampleDelayMs = 10;
    // If the samples' magnitude varies by more than this (board wasn't held still), log a
    // warning -- see calibrate()'s docstring.
    orb_real_t calibrationMaxStdDevG = orb_real_t(0.03);

    // Cosine-similarity floor for poll()'s direction match (dot product of two unit
    // vectors, so 1 = identical direction, 0 = perpendicular) -- e.g. 0.7 ~= within 45.6
    // degrees of a calibrated reference direction. Below this, no direction is reported
    // even if the deviation magnitude cleared thresholdG (better to miss a sloppy gesture
    // than fire the wrong one).
    orb_real_t minDirectionSimilarity = orb_real_t(0.7);
};

class TiltGestureDetector
{
public:
    explicit TiltGestureDetector(Qmi8658 &imu, const TiltGestureConfig &cfg = TiltGestureConfig());

    /**
     * Average cfg.calibrationSamples accelerometer readings (cfg.calibrationSampleDelayMs
     * apart) into the "planar" baseline every later poll()/pollRaw() measures deviation
     * from. Call once at boot, board resting flat/still. Logs the captured baseline and
     * the samples' standard deviation over serial -- a high stddev (see
     * cfg.calibrationMaxStdDevG) means the board was moving/being handled during
     * calibration, so the baseline may not be trustworthy; this logs a warning rather than
     * blocking/retrying (no UI to ask the user to hold still and try again yet).
     * Independent of direction calibration (see this file's header comment) -- this is
     * just the neutral-orientation reference, not the direction-vector mapping.
     */
    void calibrate();

    /** Set the "planar" baseline directly, bypassing calibrate()'s live sampling -- used at
     * boot when checkPlanarAtBoot() (main.cpp) finds the device already resting in its
     * known-good pose, to install the hardcoded default baseline (tilt_defaults.h) instead
     * of running the interactive sequence. */
    void setBaseline(orb_real_t x, orb_real_t y, orb_real_t z);

    /** Record that this normalized deviation direction (as returned by pollRaw(), i.e.
     * already unit-length) means `dir` on screen. Called by chooser.cpp's
     * calibrateDirections(); a direction with no recorded vector never matches in poll(). */
    void setMapping(orb_real_t dirX, orb_real_t dirY, orb_real_t dirZ, TiltDirection dir);

    /**
     * Log the current baseline + all 4 direction references over serial, formatted as
     * ready-to-paste C++ constant definitions matching tilt_defaults.h's shape. Called once
     * after calibrateDirections() completes a full interactive calibration (main.cpp), so
     * the printed values can become the new hardcoded defaults for future planar boots.
     */
    void logCalibrationForHardcode() const;

    /**
     * Read the IMU once and advance the hold state machine -- call once per frame/tick.
     * Mapping-independent (see this file's header comment): fires regardless of whether
     * setMapping() has been called for anything resembling this direction, so
     * calibrateDirections() can use it before any mapping exists.
     */
    RawTiltEvent pollRaw();

    /** Like pollRaw(), but matched against the learned per-direction reference vectors
     * (setMapping(), cosine similarity -- see cfg.minDirectionSimilarity) into a
     * TiltDirection -- what every other caller in this project should use. No close-enough
     * match reports TiltDirection::kNone (and never kConfirmed). */
    TiltEvent poll();

private:
    Qmi8658 &imu_;
    TiltGestureConfig cfg_;
    orb_real_t baseX_ = orb_real_t(0), baseY_ = orb_real_t(0), baseZ_ = orb_real_t(0);

    struct DirectionRef
    {
        orb_real_t x = orb_real_t(0), y = orb_real_t(0), z = orb_real_t(0);
        bool valid = false;
    };
    DirectionRef directionRefs_[kTiltDirectionCount]; // indexed by int(TiltDirection) - 1

    bool active_ = false;
    orb_real_t activeDirX_ = orb_real_t(0), activeDirY_ = orb_real_t(0), activeDirZ_ = orb_real_t(0);
    uint32_t holdStartMs_ = 0;
    bool confirmedFired_ = false;     // latched once kConfirmed has fired for this hold, so a
                                       // still-held gesture returns kHolding (not kConfirmed
                                       // again) on every subsequent poll()/pollRaw() until release
    bool longConfirmedFired_ = false; // same latch, for kConfirmedLong -- see TiltPhase's comment
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
