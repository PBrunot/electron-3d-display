/**
 * @file tilt_gesture.h
 * @brief Tilt-and-hold gesture detector, built on imu.h's QMI8658 accelerometer readings.
 *
 * Not a nudge/shake detector (a short, sharp push) -- the interaction model is a sustained
 * tilt with confirm: calibrate the resting "planar" orientation once at boot, then treat a
 * large enough SUSTAINED deviation from that baseline as a direction candidate. An arrow
 * (drawTiltArrow() below) appears immediately so the user gets feedback that a direction was
 * recognized; the action only fires once that same tilt has been HELD for kHoldConfirmMs, at
 * which point poll() returns TiltPhase::kConfirmed exactly once (an edge, not a level) so
 * callers can react to it with a plain if/switch instead of debouncing themselves. Releasing
 * the tilt (dropping back near baseline) before the hold completes cancels the candidate with
 * no action taken.
 *
 * Direction matching is full-3D-vector cosine similarity, not "pick the single axis with the
 * largest raw deviation": that dominant-axis heuristic implicitly assumes the board's rest
 * ("planar") orientation lines up cleanly with a sensor axis, which the pyramid rig's actual
 * mount angle isn't guaranteed to do. Comparing the full normalized deviation vector against
 * each calibrated reference direction via dot product (the standard nearest-direction/
 * cosine-similarity classifier -- see e.g. NXP AN3461/ST DT0140's accelerometer-tilt
 * application notes) works regardless of how gravity happens to project onto the sensor's own
 * X/Y/Z axes at rest.
 *
 * Direction mapping is LEARNED on-device, not hardcoded: chooser.cpp's calibrateDirections()
 * runs a guided sequence at boot (before the real menu appears) that asks the user to
 * tilt-and-hold each of Right/Left/Up/Down in turn, using pollRaw() below (mapping-independent)
 * to capture each one's actual deviation vector, then calls setMapping() to record it as that
 * direction's reference. Until calibrated, every direction has no reference vector and poll()
 * reports no direction for any tilt -- a safe default, not a wrong-direction risk.
 */
#pragma once

#include <cstdint>

class Display;

#include "ux/imu.h"
#include "physics/orbitals.h" // orb_real_t
#include "config/visual_constants.h" // kTiltArrowMarginPx/LengthPx/HalfWidthPx

enum class TiltDirection : uint8_t
{
    kNone,
    kLeft,
    kRight,
    kUp,
    kDown,
};
inline constexpr int kTiltDirectionCount = 4; // kLeft..kDown, i.e. excluding kNone

/// Human-readable name, for logging only.
const char *tiltDirectionName(TiltDirection dir);

enum class TiltPhase : uint8_t
{
    kIdle,     ///< No candidate direction currently held.
    kHolding,  ///< A direction is held, still short of kHoldConfirmMs.
    kConfirmed ///< Hold just crossed kHoldConfirmMs -- fires exactly once per hold (an edge).
};

struct TiltEvent
{
    TiltDirection direction = TiltDirection::kNone;
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0; ///< Elapsed hold time -- for a future progress animation on the arrow.
};

/**
 * @brief Like TiltEvent, but the raw normalized deviation-from-baseline direction (a unit
 *        vector in board-local accelerometer axes) instead of a mapped TiltDirection.
 *
 * Valid only while phase != kIdle. Used by calibrateDirections() to capture each target
 * direction's reference vector for poll()/TiltEvent to match against afterward.
 */
struct RawTiltEvent
{
    orb_real_t dirX = orb_real_t(0), dirY = orb_real_t(0), dirZ = orb_real_t(0);
    TiltPhase phase = TiltPhase::kIdle;
    uint32_t holdMs = 0;
};

struct TiltGestureConfig
{
    orb_real_t thresholdG = orb_real_t(0.28); ///< Deviation magnitude to arm a candidate direction.
    /// Deviation must drop below this to re-arm -- hysteresis so a reading sitting right at
    /// thresholdG doesn't chatter.
    orb_real_t releaseG = orb_real_t(0.18);
    /// Sustained-hold duration required to confirm a tilt.
    uint32_t holdConfirmMs = 1000;

    /// Baseline ("planar") calibration sample count -- long/slow enough (calibrationSamples *
    /// calibrationSampleDelayMs =~ 1s) for a stable zero-tilt reference, per calibration
    /// write-ups for this class of sensor (e.g. MPU6050 calibration guides).
    int calibrationSamples = 100;
    uint32_t calibrationSampleDelayMs = 10;
    /// If the samples' magnitude varies by more than this (board wasn't held still), log a
    /// warning -- see calibrate()'s docstring.
    orb_real_t calibrationMaxStdDevG = orb_real_t(0.03);

    /// Cosine-similarity floor for poll()'s direction match (dot product of two unit vectors,
    /// so 1 = identical direction, 0 = perpendicular) -- e.g. 0.7 ~= within 45.6 degrees of a
    /// calibrated reference direction. Below this, no direction is reported even if the
    /// deviation magnitude cleared thresholdG (better to miss a sloppy gesture than fire the
    /// wrong one).
    orb_real_t minDirectionSimilarity = orb_real_t(0.7);
};

class TiltGestureDetector
{
public:
    explicit TiltGestureDetector(Qmi8658 &imu, const TiltGestureConfig &cfg = TiltGestureConfig());

    /**
     * @brief Average cfg.calibrationSamples accelerometer readings into the "planar" baseline
     *        every later poll()/pollRaw() measures deviation from.
     *
     * Call once at boot, board resting flat/still. Logs the captured baseline and the
     * samples' standard deviation over serial; a high stddev (see cfg.calibrationMaxStdDevG)
     * means the board was moving during calibration, so the baseline may not be trustworthy --
     * logged as a warning rather than blocking/retrying. Independent of direction calibration
     * -- this is just the neutral-orientation reference, not the direction-vector mapping.
     */
    void calibrate();

    /// Set the "planar" baseline directly, bypassing calibrate()'s live sampling -- used at
    /// boot when Qmi8658::checkPlanarAtBoot() (imu.h) finds the device already resting in its
    /// known-good pose, to install the hardcoded default baseline (config/hardware_constants.h) instead
    /// of running the interactive sequence.
    void setBaseline(orb_real_t x, orb_real_t y, orb_real_t z);

    /// Record that this normalized deviation direction (as returned by pollRaw(), i.e.
    /// already unit-length) means `dir` on screen. Called by chooser.cpp's
    /// calibrateDirections(); a direction with no recorded vector never matches in poll().
    void setMapping(orb_real_t dirX, orb_real_t dirY, orb_real_t dirZ, TiltDirection dir);

    /**
     * @brief Log the current baseline + all 4 direction references over serial, formatted as
     *        ready-to-paste C++ constant definitions matching config/hardware_constants.h's shape.
     *
     * Called once after calibrateDirections() completes a full interactive calibration
     * (main.cpp), so the printed values can become the new hardcoded defaults for future
     * planar boots.
     */
    void logCalibrationForHardcode() const;

    /// Read the IMU once and advance the hold state machine -- call once per frame/tick.
    /// Mapping-independent: fires regardless of whether setMapping() has been called for
    /// anything resembling this direction, so calibrateDirections() can use it before any
    /// mapping exists.
    RawTiltEvent pollRaw();

    /// Like pollRaw(), but matched against the learned per-direction reference vectors
    /// (setMapping(), cosine similarity -- see cfg.minDirectionSimilarity) into a
    /// TiltDirection -- what every other caller in this project should use. No close-enough
    /// match reports TiltDirection::kNone (and never kConfirmed).
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
    bool confirmedFired_ = false; // latched once kConfirmed has fired for this hold, so a
                                   // still-held gesture returns kHolding (not kConfirmed
                                   // again) on every subsequent poll()/pollRaw() until release
};

/// Draw a single filled triangle arrow at the center of the screen edge `dir` points toward
/// (kNone draws nothing). Plain/static -- no animation or text; kTiltArrow*Px above are the
/// only tuning knobs needed to add either later without touching the rasterizer.
void drawTiltArrow(Display &display, TiltDirection dir, uint16_t color);

/// Axis-aligned pixel rectangle, [x, x+w) x [y, y+h). An empty rect (w == 0 or h == 0) means
/// "nothing to draw/erase".
struct PixelRect
{
    int x = 0, y = 0, w = 0, h = 0;
};

/**
 * @brief Bounding box of drawTiltArrow(dir)'s triangle, clipped to the display -- computed
 *        from the exact same vertices drawTiltArrow() rasterizes, so the two can never
 *        disagree about where the arrow actually lands.
 *
 * Lets a caller that draws its own static background (ux/chooser.cpp's menu loop) erase a
 * previously-drawn arrow by restoring just this rectangle instead of redrawing the whole
 * screen every frame. kNone returns an empty rect.
 */
PixelRect tiltArrowBounds(TiltDirection dir);

/**
 * @brief Snapshot the display's current pixels under each direction's arrow bounding box
 *        (tiltArrowBounds()) into a small internal cache (well under 1KB per direction --
 *        nowhere near the size of a whole-background cache, see splash_bitmap.cpp's header
 *        comment for why THAT doesn't fit the CYD's internal RAM), so a later
 *        restoreArrowBackground() can erase a drawn arrow without redrawing -- or re-decoding
 *        -- anything bigger.
 *
 * Call this once right after the background an arrow might be drawn over is freshly painted
 * (e.g. ux/chooser.cpp's menu redraw), before any arrow is drawn on top of it -- capturing
 * after an arrow was drawn would cache the arrow itself instead of what's behind it.
 */
void captureArrowBackgrounds(Display &display);

/// Restore the cached background (see captureArrowBackgrounds()) under `dir`'s arrow -- the
/// erase counterpart to drawTiltArrow(). No-op for kNone or if captureArrowBackgrounds() was
/// never called.
void restoreArrowBackground(Display &display, TiltDirection dir);
