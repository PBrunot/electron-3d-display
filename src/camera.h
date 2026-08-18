/**
 * @file camera.h
 * @brief Three-axis tumble camera (yaw/tilt/roll) + orthographic projection + point-cloud
 *        rendering pipeline (fade, proton marker, alpha-blended points, fly-over easing).
 *
 * All three axes are necessary: yaw+tilt alone leaves any point near the world Y axis (e.g.
 * the core of a 2p_y or 3d_x2-y2 lobe) pinned to the screen's vertical centerline forever --
 * a persistent streak, not a transient one. Roll is what guarantees no point is invariant in
 * any screen coordinate.
 *
 * Panel geometry (mirror/rotation) is set entirely in display.cpp's esp_lcd_panel_mirror()
 * call and left untouched by this module.
 *
 * renderPointsUniform()/renderPointsColored()/renderScene()/flyOver() below are templates
 * (over the point type, and flyOver() also over its title-drawing callable) and so stay
 * defined here in the header, unlike every non-template function in this file (moved to
 * camera.cpp): a template's definition has to be visible at each call site's translation
 * unit, and orbital_view.cpp/atom_view.cpp each instantiate these with different point
 * types, so there is no single .cpp they could live in instead.
 */
#pragma once

#include <cstdint>

#include "display.h"
#include "orbitals.h" // orb_real_t
#include "overlay.h"

// ============================================================================================
// Tunable constants
// ============================================================================================

// --- Tumble camera pacing ---
constexpr orb_real_t kCameraAngleStep = orb_real_t(0.030); ///< Yaw change per frame.
constexpr orb_real_t kCameraTiltStep = orb_real_t(0.023);  ///< Tilt change per frame.
constexpr orb_real_t kCameraRollStep = orb_real_t(0.017);  ///< Roll change per frame.
// The three steps are deliberately non-resonant with each other, so the tumble doesn't fall
// into a short repeating loop.

/// Tilt/roll start away from the degenerate all-zero pose (where yaw alone can't move
/// axis-aligned lobes at all), so even the first frame after boot isn't axis-locked.
constexpr orb_real_t kCameraTiltStart = orb_real_t(0.9);
constexpr orb_real_t kCameraRollStart = orb_real_t(2.1);

constexpr orb_real_t kTwoPi = orb_real_t(2) * kOrbitalPi;

/// Side length in pixels of the proton marker drawn by drawProtonMarker().
constexpr int kProtonMarkerSize = 3;

// --- Point transparency + frame persistence ---
// Every point alpha-blends toward its own color instead of overwriting the pixel outright, so
// overlapping points converge toward full brightness rather than the last-drawn one flatly
// replacing whatever was there. Expressed as a /256 fixed-point fraction (Display::
// blendColor565()'s alphaQ8) to keep every per-pixel color op in this project integer-only.
/// Blend weight toward each point's own color; 256 = fully opaque (no blend-through of the
/// pixel underneath). Kept fully opaque so two different-colored points landing on the same
/// pixel across frames (e.g. an orbital's orange and blue lobes) each show their exact
/// intended color rather than muddying into an intermediate hue -- per-point brightness
/// already comes from the level encoded into the point's own color (see orbital_presets.cpp's
/// orbitalLevelToColor565() / atom_cloud.cpp's shell brighten/dim).
constexpr uint16_t kElectronAlphaQ8 = 256;

/// Fraction of the previous frame buffer's brightness kept each frame (Display::
/// fadeColor565()) instead of hard-clearing it, so points leave a brief trailing glow and
/// skipped "buzz" points fade out instead of vanishing outright. Tuned high enough that a hit
/// pixel's glow stays alive between re-hits as points sweep across the screen during
/// rotation, filling in gaps that would otherwise read as flicker.
constexpr uint16_t kPersistenceKeepQ8 = 150; // 150/256 kept per frame (~0.59)

// --- Hidden-points "buzz" flicker (feeds renderPointsColored()'s buzzThreshold param) ---
/// Fraction of points skipped on any given frame (a different pseudo-random subset each
/// frame), so the cloud reads as a buzzing/flickering probability cloud rather than a static
/// fixed set of dots. Shared by orbital_view.cpp and atom_view.cpp so the effect reads the
/// same across both cloud types.
constexpr orb_real_t kHiddenPointsFraction = orb_real_t(0.25);
/// Threshold derived from kHiddenPointsFraction for renderPointsColored()'s 16-bit hash. 0
/// (rather than this constant) disables buzz outright, since the hash is unsigned and never
/// compares less than 0.
constexpr uint32_t kHiddenPointsThreshold = uint32_t(kHiddenPointsFraction * orb_real_t(65536));

// --- Fly-over easing pace ---
// Camera starts at baseScale * factor and eases to baseScale over `frames` frames. Boot intro
// is slower/more dramatic than a preset/element switch, which is more dramatic than a
// mid-scene zoom excursion.
constexpr orb_real_t kIntroStartScaleFactor = orb_real_t(12.0);  ///< Boot intro start-scale multiplier.
constexpr int kIntroFrames = 70;                                 ///< Boot intro ease duration, frames.
constexpr orb_real_t kSwitchStartScaleFactor = orb_real_t(10.0); ///< Preset/element switch start-scale multiplier.
constexpr int kSwitchTransitionFrames = 18;                      ///< Preset/element switch ease duration, frames.

// --- Random zoom excursions (steady-state loop) ---
// At randomized intervals (re-rolled after each one), ease from the current breathing scale
// to a randomized target and back -- layered on top of the constant sine-wave breathing so
// the animation doesn't read as purely mechanical.
constexpr int kZoomExcursionMinIntervalFrames = 150;             ///< Shortest gap between excursions, frames.
constexpr int kZoomExcursionMaxIntervalFrames = 400;             ///< Longest gap between excursions, frames.
constexpr orb_real_t kZoomExcursionScaleMinFactor = orb_real_t(0.4); ///< Excursion "zoom out" target multiplier.
/// Excursion "zoom in" target multiplier. Scale multiplies screen-space offset directly
/// (projectPoint()), so a larger factor is the "dive IN" direction: the outer subshell's own
/// radius grows well past the screen edge and only near-center points stay visible, reading
/// as flying through/inside the cloud rather than a simple zoom-in on the whole shape.
/// Matches kIntroStartScaleFactor -- the same "extremely close" magnitude already validated
/// on hardware via the boot intro.
constexpr orb_real_t kZoomExcursionScaleMaxFactor = orb_real_t(12.0);
/// Excursion ease duration, frames. A deeper dive (see kZoomExcursionScaleMaxFactor) covers
/// more visual distance per frame, so needs more frames to read as a deliberate dive-through
/// rather than a jarring flash.
constexpr int kZoomExcursionEaseFrames = 45;
/// Breathing zoom's angular speed; independent phase from kCameraAngleStep.
constexpr orb_real_t kZoomAngleStep = orb_real_t(0.016);

// ============================================================================================

/// Running yaw/tilt/roll angles for the tumble animation.
struct CameraState
{
    orb_real_t yaw = orb_real_t(0);
    orb_real_t tilt = kCameraTiltStart;
    orb_real_t roll = kCameraRollStart;
};

/// Advance all three angles by one frame's step, wrapping into [0, 2*pi).
void stepCamera(CameraState *cam);

/// Precomputed sin/cos for one frame's rotation -- reused for every point that frame.
struct RotationTrig
{
    orb_real_t cosYaw, sinYaw, cosTilt, sinTilt, cosRoll, sinRoll;
};

RotationTrig computeRotationTrig(const CameraState &cam);

/**
 * @brief Rotate (yaw, then tilt, then roll) and orthographically project one point about the
 *        screen center.
 * @return false if the point is off-screen (*outSx/outSy* left unwritten), true with integer
 *         screen coordinates written to *outSx/outSy* otherwise.
 * @note Depth after yaw alone (rz1) is computed only to feed the tilt step, then dropped --
 *       no depth-sort/z-buffer anywhere in this pipeline, which is fine for a sparse point
 *       cloud. Rounds to nearest (not truncation) to avoid biasing points toward the screen's
 *       centerlines.
 */
bool projectPoint(orb_real_t x, orb_real_t y, orb_real_t z, const RotationTrig &t, orb_real_t scale, int *outSx,
                  int *outSy);

/**
 * @brief Draw the small proton marker centered on screen.
 * @note Drawn fully opaque (Display::packColor565() overwrite, no blending) -- the nucleus is
 *       one literal particle, not a probability cloud.
 */
void drawProtonMarker(uint16_t *frameBuf, uint16_t color);

/// Fade every pixel of `frameBuf` toward black by kPersistenceKeepQ8. Not a template (doesn't
/// depend on PointT), so its body lives in camera.cpp.
void fadeFrameBuffer(uint16_t *frameBuf);

/**
 * @brief Render `count` points, all alpha-blended toward a single `color` (see
 *        kElectronAlphaQ8).
 *
 * `points` may be any type with public x/y/z orb_real_t members (both AtomPoint and
 * OrbitalPoint qualify). `frameBuf` is NOT cleared by this function -- caller clears/fades
 * first. Convenience wrapper for a still-uncolored cloud; per-point coloring (phase for
 * orbitals, shell for atoms) uses renderPointsColored()/renderScene() below.
 */
template <typename PointT>
void renderPointsUniform(uint16_t *frameBuf, const PointT *points, int count, uint16_t color, const RotationTrig &t,
                         orb_real_t scale)
{
    for (int i = 0; i < count; i++)
    {
        int sx, sy;
        if (projectPoint(points[i].x, points[i].y, points[i].z, t, scale, &sx, &sy))
        {
            int idx = sy * Display::kDisplayWidth + sx;
            frameBuf[idx] = Display::blendColor565(frameBuf[idx], color, kElectronAlphaQ8);
        }
    }
}

/**
 * @brief Render `count` points with per-point colors, alpha-blended toward each point's own
 *        color (see kElectronAlphaQ8), plus an optional "buzz" flicker.
 *
 * A point is skipped this frame if a cheap per-point/per-frame hash falls below
 * `buzzThreshold` (0, the default, disables buzz entirely, since the hash is unsigned and
 * "hash < 0" is never true). Uses two 32-bit multiplicative hash constants (668265261 /
 * 374761393, Bob Jenkins'/xxHash's) on the point index and a per-frame salt.
 */
template <typename PointT>
void renderPointsColored(uint16_t *frameBuf, const PointT *points, const uint16_t *colors, int count,
                         const RotationTrig &t, orb_real_t scale, uint32_t frameSalt = 0,
                         uint32_t buzzThreshold = 0)
{
    for (int i = 0; i < count; i++)
    {
        uint32_t hv = ((uint32_t(i) * 668265261u + frameSalt * 374761393u) >> 16) & 0xFFFFu;
        if (hv < buzzThreshold)
            continue;
        int sx, sy;
        if (projectPoint(points[i].x, points[i].y, points[i].z, t, scale, &sx, &sy))
        {
            int idx = sy * Display::kDisplayWidth + sx;
            frameBuf[idx] = Display::blendColor565(frameBuf[idx], colors[i], kElectronAlphaQ8);
        }
    }
}

/**
 * @brief Fade, draw the proton marker, then every point at the given camera pose/scale.
 *
 * The nucleus is drawn BEFORE the points (not after) so a point that projects onto the same
 * pixel alpha-blends over it like any other pixel. Does NOT draw title/FPS/scale-bar text or
 * present the frame -- caller does both.
 */
template <typename PointT>
void renderScene(uint16_t *frameBuf, const PointT *points, const uint16_t *colors, int count, uint16_t protonColor,
                 const CameraState &camera, orb_real_t scale, uint32_t frameSalt = 0, uint32_t buzzThreshold = 0)
{
    fadeFrameBuffer(frameBuf);
    drawProtonMarker(frameBuf, protonColor);
    RotationTrig trig = computeRotationTrig(camera);
    renderPointsColored(frameBuf, points, colors, count, trig, scale, frameSalt, buzzThreshold);
}

/// Uniform random float in [0, 1), via the hardware RNG (esp_random()) -- used only for
/// animation timing/targets (zoom excursions, chooser backdrops), not for anything that needs
/// to be reproducible (that's XorShift32's job, see pointcloud.h).
orb_real_t randomUnit();

orb_real_t randomUniform(orb_real_t lo, orb_real_t hi);

/// Random frame countdown until the next zoom excursion (see kZoomExcursion* above).
int nextZoomExcursionCountdown();

/**
 * @brief Uniform random int in [0, count), guaranteed != current.
 *
 * Used by AtomView/OrbitalView's idle-timeout auto-advance to pick a genuinely different
 * element/orbital preset rather than risking a no-op "jump" back to the same one.
 * @note count must be >= 2 (undefined otherwise -- there'd be nothing else to jump to).
 */
int randomIndexExcluding(int current, int count);

/**
 * @brief Ease the projection scale from startScale to endScale over `frames` frames, rendering
 *        and presenting each one, advancing `camera` by one step every frame.
 *
 * Shared by the boot intro, nudge-triggered switches, and random zoom excursions. `drawTitle`
 * is a callable `(uint16_t* frameBuf, int x, int y, uint16_t color) -> void` -- a plain
 * single-color title for orbital_view, a per-shell-colored multi-segment one for atom_view;
 * templated instead of a fixed function pointer so both fit without an indirection cost on
 * this per-point-cheap-but-still-hot path. `buzzThreshold` (see renderPointsColored())
 * defaults to 0 (no flicker); orbital_view.cpp's and atom_view.cpp's own local
 * flyOver-equivalents (proton-marker-redraw variants of this one) pass kHiddenPointsThreshold
 * explicitly so buzz stays active through transitions too.
 */
template <typename PointT, typename TitleDrawFn>
void flyOver(Display &display, const PointT *points, const uint16_t *colors, int count, TitleDrawFn drawTitle,
             uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, CameraState *camera,
             orb_real_t startScale, orb_real_t endScale, int frames, uint32_t buzzThreshold = 0)
{
    for (int i = 0; i < frames; i++)
    {
        orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
        orb_real_t scale = startScale + (endScale - startScale) * t;

        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderScene(display.getFrameBuf(), points, colors, count, protonColor, *camera, scale, uint32_t(i),
                    buzzThreshold);
        drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
        display.presentFrame();

        stepCamera(camera);
    }
}
