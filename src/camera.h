// Three-axis tumble camera (yaw about Y, tilt about X, roll about Z) + orthographic
// projection + proton-marker overlay. Port of micropython/device_render_common.py's
// render_points()/render_frame() -- see that file's module docstring for why all three
// axes are needed: yaw+tilt alone leaves any point near the world Y axis (e.g. the core
// of a 2p_y or 3d_x2-y2 lobe) pinned to the screen's vertical centerline forever, a
// persistent streak, not a transient one. A third independent axis (roll) is what
// guarantees no point is invariant in any screen coordinate.
//
// Unlike the MicroPython port, this stays plain orb_real_t (float) throughout -- no
// Q8 fixed-point/viper trick. That trick exists only to escape MicroPython's interpreter
// overhead per point (CLAUDE.md section 3's whole reason for choosing C++ over
// MicroPython in the first place); compiled C++ trig has no such overhead.
//
// Panel geometry (mirror/rotation) is set entirely in display.cpp's esp_lcd_panel_mirror()
// call and left untouched by this module -- CLAUDE.md's "Geometria... ancora da
// verificare" note flags this as still-open on this exact unit via ESP-IDF (the data
// collected so far was contaminated by the now-fixed G/B color bug), so this file does
// NOT add micropython/display.py's extra software 180-degree flip on top of it -- that
// flip was verified for a DIFFERENT rendering path (framebuf + machine.SPI), not this
// one. Revisit together if/when this unit's own corner-calibration test is redone.
//
// renderPointsUniform()/renderPointsColored()/renderScene()/flyOver() below are templates
// (over the point type, and flyOver() also over its title-drawing callable) and so stay
// defined here in the header, unlike every non-template function in this file (moved to
// camera.cpp) -- a template's definition has to be visible at each call site's translation
// unit, and orbital_view.cpp/atom_view.cpp each instantiate these with different point
// types, so there is no single .cpp they could live in instead.
#pragma once

#include <cstdint>

#include "display.h"
#include "orbitals.h" // orb_real_t
#include "overlay.h"

constexpr orb_real_t kCameraAngleStep = orb_real_t(0.030); // yaw/frame
constexpr orb_real_t kCameraTiltStep = orb_real_t(0.023);  // tilt/frame
constexpr orb_real_t kCameraRollStep = orb_real_t(0.017);  // roll/frame
// Non-resonant vs. each other and vs. kCameraAngleStep on purpose (see
// device_render_common.py's ANGLE_STEP/TILT_ANGLE_STEP/ROLL_ANGLE_STEP comment) --
// avoids the tumble falling into a short repeating loop.

// tilt/roll start away from the degenerate all-zero pose (where yaw alone can't move
// axis-aligned lobes at all), so even the first frame after boot isn't axis-locked.
constexpr orb_real_t kCameraTiltStart = orb_real_t(0.9);
constexpr orb_real_t kCameraRollStart = orb_real_t(2.1);

constexpr orb_real_t kTwoPi = orb_real_t(2) * kOrbitalPi;

/** Running yaw/tilt/roll angles for the tumble animation. */
struct CameraState {
    orb_real_t yaw = orb_real_t(0);
    orb_real_t tilt = kCameraTiltStart;
    orb_real_t roll = kCameraRollStart;
};

/** Advance all three angles by one frame's step, wrapping into [0, 2*pi). */
void stepCamera(CameraState* cam);

/** Precomputed sin/cos for one frame's rotation -- reused for every point that frame. */
struct RotationTrig {
    orb_real_t cosYaw, sinYaw, cosTilt, sinTilt, cosRoll, sinRoll;
};

RotationTrig computeRotationTrig(const CameraState& cam);

/**
 * Rotate (yaw, then tilt, then roll) and orthographically project one point about the
 * screen center. Returns false (point off-screen, *outSx/outSy* unwritten) or true with
 * integer screen coordinates written to *outSx/outSy*. Depth after yaw alone (rz1) is
 * computed only to feed the tilt step, then dropped -- no depth-sort/z-buffer anywhere in
 * this pipeline (CLAUDE.md section 5: fine for a sparse point cloud). Rounds to nearest
 * (not truncation) to avoid biasing points toward the screen's centerlines -- see
 * device_render_common.py's render_points() docstring for the measured size of that bias
 * under plain int() truncation.
 */
bool projectPoint(orb_real_t x, orb_real_t y, orb_real_t z, const RotationTrig& t, orb_real_t scale, int* outSx,
                   int* outSy);

constexpr int kProtonMarkerSize = 3;

/** Draw the small proton marker centered on screen, matching device_render_common.py's PROTON_SIZE.
 * Drawn fully opaque (Display::packColor565() overwrite, no blending) -- the nucleus is one literal
 * particle, not a probability cloud, matching pc/viewer_common.py's draw_nucleus(). */
void drawProtonMarker(uint16_t* frameBuf, uint16_t color);

// --- Transparency + persistence (port of pc/viewer_common.py's ELECTRON_ALPHA/PERSISTENCE_DECAY) ---
//
// Every point alpha-blends toward its own color instead of overwriting the pixel outright, so
// overlapping points (this frame, or via persistence below, recently-drawn nearby ones) converge
// toward full brightness and a point from an inner layer shows faintly through one from an outer
// layer that only partially overwrote it -- the translucent point-cloud look. Expressed as a /256
// fixed-point fraction (Display::blendColor565()'s alphaQ8) rather than pc/viewer_common.py's float
// ELECTRON_ALPHA=0.8, to keep every per-pixel color op in this project integer-only.
// Raised from pc/viewer_common.py's ELECTRON_ALPHA=0.8 (205/256): during rotation a given point
// rarely lands on the exact same pixel two frames running, so it gets essentially one blend
// toward full brightness before the persistence fade below starts pulling that pixel back down --
// the cloud reads visibly dimmer in motion than in a static single-frame render. A stronger alpha
// makes each individual hit closer to full brightness, which is where the perceived dimming
// during animation/rotation actually comes from (not a rendering bug -- see kPersistenceKeepQ8).
constexpr uint16_t kElectronAlphaQ8 = 235; // ~0.92 * 256

// Each frame fades the previous buffer toward black (Display::fadeColor565()) instead of hard-
// clearing it, so points leave a brief trailing glow and skipped "buzz" points fade out instead of
// vanishing outright. Port of pc/viewer_common.py's PERSISTENCE_DECAY -- that constant's name is
// actually the fraction KEPT per frame (not removed), kept here under a less confusing name.
// Raised from pc/viewer_common.py's 100/256 (~0.39): slower decay keeps a hit pixel's glow alive
// longer between re-hits as points sweep across the screen during rotation, filling in the gaps
// that otherwise read as fading -- same motivation as kElectronAlphaQ8 above, tuned together.
constexpr uint16_t kPersistenceKeepQ8 = 150; // 150/256 kept per frame (~0.59)

/** Fade every pixel of `frameBuf` toward black by kPersistenceKeepQ8 -- see the constant's comment.
 * Not a template (doesn't depend on PointT), so its body lives in camera.cpp. */
void fadeFrameBuffer(uint16_t* frameBuf);

/**
 * Render `count` points from `points` (any type with public x/y/z orb_real_t members --
 * both AtomPoint and OrbitalPoint qualify) into `frameBuf`, all alpha-blended toward a single
 * `color` (see kElectronAlphaQ8 above). frameBuf is NOT cleared by this function -- caller
 * clears/fades first. Convenience wrapper for a still-uncolored cloud (used by
 * atom_view_test.h's build); per-point coloring (phase for orbitals, shell for atoms) uses
 * renderPointsColored()/renderScene() below.
 */
template <typename PointT>
void renderPointsUniform(uint16_t* frameBuf, const PointT* points, int count, uint16_t color, const RotationTrig& t,
                          orb_real_t scale) {
    for (int i = 0; i < count; i++) {
        int sx, sy;
        if (projectPoint(points[i].x, points[i].y, points[i].z, t, scale, &sx, &sy)) {
            int idx = sy * Display::kDisplayWidth + sx;
            frameBuf[idx] = Display::blendColor565(frameBuf[idx], color, kElectronAlphaQ8);
        }
    }
}

/**
 * Render `count` points with PER-POINT colors (colors[i] for points[i]), alpha-blended toward
 * each point's own color (see kElectronAlphaQ8 above), plus an optional "buzz" flicker: a point
 * is skipped this frame if a cheap per-point/per-frame hash falls below buzzThreshold (0 -- the
 * default -- disables buzz entirely: since the hash is unsigned, "hash < 0" is never true, so
 * every point always draws). Port of device_render_common.py's render_points() viper kernel,
 * minus the Q8 fixed-point (see this file's header comment) and using the same multiplicative
 * hash constants (668265261/374761393 -- Bob Jenkins'/xxHash's 32-bit constants) on the point
 * index and a per-frame salt.
 */
template <typename PointT>
void renderPointsColored(uint16_t* frameBuf, const PointT* points, const uint16_t* colors, int count,
                          const RotationTrig& t, orb_real_t scale, uint32_t frameSalt = 0,
                          uint32_t buzzThreshold = 0) {
    for (int i = 0; i < count; i++) {
        uint32_t hv = ((uint32_t(i) * 668265261u + frameSalt * 374761393u) >> 16) & 0xFFFFu;
        if (hv < buzzThreshold)
            continue;
        int sx, sy;
        if (projectPoint(points[i].x, points[i].y, points[i].z, t, scale, &sx, &sy)) {
            int idx = sy * Display::kDisplayWidth + sx;
            frameBuf[idx] = Display::blendColor565(frameBuf[idx], colors[i], kElectronAlphaQ8);
        }
    }
}

/**
 * Fade (see fadeFrameBuffer()), draw the proton marker, then every point at the given camera
 * pose/scale. Nucleus drawn BEFORE the points (not after) so a point that projects onto the same
 * pixel alpha-blends over it like any other pixel, matching pc/viewer_common.py's render_frame()
 * draw order. Does NOT draw title/FPS/scale-bar text or present the frame -- caller does both,
 * matching device_render_common.py's render_frame()/run()-loop division of labor (both its
 * fly_over() and each viewer's steady-state loop call render_frame() then separately layer text
 * on top).
 */
template <typename PointT>
void renderScene(uint16_t* frameBuf, const PointT* points, const uint16_t* colors, int count, uint16_t protonColor,
                  const CameraState& camera, orb_real_t scale, uint32_t frameSalt = 0, uint32_t buzzThreshold = 0) {
    fadeFrameBuffer(frameBuf);
    drawProtonMarker(frameBuf, protonColor);
    RotationTrig trig = computeRotationTrig(camera);
    renderPointsColored(frameBuf, points, colors, count, trig, scale, frameSalt, buzzThreshold);
}

// Fly-over (see flyOver() below): camera starts at baseScale * factor and eases to
// baseScale over `frames` frames. Boot intro is slower/more dramatic than a preset/element
// switch, which is more dramatic than a mid-scene zoom excursion. Ported 1:1 from
// device_render_common.py's INTRO_*/SWITCH_*/ZOOM_EXCURSION_* constants.
constexpr orb_real_t kIntroStartScaleFactor = orb_real_t(12.0);
constexpr int kIntroFrames = 70;
constexpr orb_real_t kSwitchStartScaleFactor = orb_real_t(10.0);
constexpr int kSwitchTransitionFrames = 18;

// Random zoom excursions during the steady-state loop: at randomized intervals (re-rolled
// after each one), ease from the current breathing scale to a randomized target and back
// -- layered on top of the constant sine-wave breathing so the animation doesn't read as
// purely mechanical.
constexpr int kZoomExcursionMinIntervalFrames = 150;
constexpr int kZoomExcursionMaxIntervalFrames = 400;
constexpr orb_real_t kZoomExcursionScaleMinFactor = orb_real_t(0.4);
// Scale multiplies screen-space offset directly (projectPoint()), so a LARGER factor here
// is the "dive IN" direction: the outer subshell's own radius grows well past the screen
// edge and only near-center points stay visible, reading as flying through/inside the
// cloud rather than a simple zoom-in on the whole shape. Raised from 5.0 (still visibly a
// zoom-in, but not dramatic enough per feedback) to 12.0 -- matches kIntroStartScaleFactor
// above, an already-validated-on-hardware "extremely close" magnitude (the boot intro
// starts there and eases out).
constexpr orb_real_t kZoomExcursionScaleMaxFactor = orb_real_t(12.0);
// Raised from 30: a deeper dive covers more visual distance per frame, so needs more
// frames to read as a deliberate dive-through rather than a jarring flash.
constexpr int kZoomExcursionEaseFrames = 45;
constexpr orb_real_t kZoomAngleStep = orb_real_t(0.016); // breathing zoom's angular speed,
                                                          // independent phase from kCameraAngleStep

/** Uniform random float in [0, 1), via the hardware RNG (esp_random()) -- used only for
 * animation timing/targets (zoom excursions, chooser backdrops), NOT for anything that
 * needs to match the MicroPython/PC ports bit-for-bit (that's XorShift32's job, see
 * pointcloud.h). */
orb_real_t randomUnit();

orb_real_t randomUniform(orb_real_t lo, orb_real_t hi);

/** Random frame countdown until the next zoom excursion (see kZoomExcursion* above). */
int nextZoomExcursionCountdown();

/**
 * Uniform random int in [0, count), guaranteed != current -- used by AtomView/OrbitalView's
 * idle-timeout auto-advance (see each view's kIdleJumpUs) to pick a genuinely different
 * element/orbital preset rather than risking a no-op "jump" back to the same one. count must
 * be >= 2 (undefined otherwise -- there'd be nothing else to jump to).
 */
int randomIndexExcluding(int current, int count);

/**
 * Ease the projection scale from startScale to endScale over `frames` frames, rendering +
 * presenting each one, advancing `camera` by one step every frame. Shared by the boot
 * intro, nudge-triggered switches, and random zoom excursions. `drawTitle` is a callable
 * `(uint16_t* frameBuf, int x, int y, uint16_t color) -> void` -- a plain single-color
 * title for orbital_view (M3), a per-shell-colored multi-segment one for atom_view (M4);
 * templated instead of a fixed function pointer so both fit without an indirection cost
 * on this per-point-cheap-but-still-hot path. `buzzThreshold` (see renderPointsColored())
 * defaults to 0 (no flicker) -- orbital_view passes its BUZZ_FRACTION-derived threshold
 * explicitly so buzz stays active through transitions too; atom_view's static cloud has no
 * use for it and leaves it at the default. Port of device_render_common.py's fly_over().
 */
template <typename PointT, typename TitleDrawFn>
void flyOver(Display& display, const PointT* points, const uint16_t* colors, int count, TitleDrawFn drawTitle,
             uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, CameraState* camera,
             orb_real_t startScale, orb_real_t endScale, int frames, uint32_t buzzThreshold = 0) {
    for (int i = 0; i < frames; i++) {
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
