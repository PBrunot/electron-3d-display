// Consolidated adjustable visual/animation tuning knobs: colors, on-screen sizes/positions,
// and animation pacing (speeds, durations, frame counts) for every viewer/menu. Constants
// that AREN'T really adjustable-by-eye stay in their original module instead of moving here:
// domain color-mapping tables (Display's own palette in render/display.h, atom_cloud.h's
// kAtomShellRgb/orbital_library.h's per-preset phase colors), asset-shape constants tied to a
// generated header (equation_bitmap.h/splash_bitmap.h's width/height), and algorithm/physics
// parameters (orbitals.h's table resolution, slater.h's Madelung data).
//
// Headers that used to define some of these directly (camera.h, overlay.h, ticker.h,
// orbital_presets.h, atom_cloud.h, tilt_gesture.h) now #include this file instead, so every
// existing include site keeps seeing the same names with no call-site changes needed.
#pragma once

#include <cstdint>

#include "physics/orbitals.h"       // orb_real_t
#include "render/display.h"         // Display, packColor565
#include "render/equation_bitmap.h" // kEquationBitmapWidth/Height, for the orbital intro layout below
#include "sdkconfig.h"              // CONFIG_IDF_TARGET_ESP32

// ============================================================================================
// Shared across both viewers (dedup of what used to be near-identical per-file copies)
// ============================================================================================

/// Accent color for the app's few highlight moments (tilt arrows, scrolling tickers, the
/// dissection shell notation, the chooser menu's calibration arrow) -- one color so they all
/// read as the same "highlight", not several unrelated ambers.
inline constexpr uint16_t kAccentColor = Display::packColor565(255, 210, 60);

/// Solid nucleus marker color, shared by both viewers' main render loops.
inline constexpr uint16_t kProtonColor = Display::kColorRed;

/// Title/scale-bar text color, both viewers.
inline constexpr uint16_t kTextColor = Display::kColorWhite;
inline constexpr uint16_t kScaleBarColor = Display::kColorLightGrey;

/// Frame count between FPS log lines, both viewers' main loops. Kept high (not every-frame,
/// not even every 100) since the moving-average timings in debug/frame_stats.h already
/// smooth out single-frame jitter -- logging that often over serial would just be spam.
inline constexpr int kFpsUpdateInterval = 1000;

/// Idle time with no confirmed tilt input before a viewer auto-advances to a random
/// preset/element. Same value for both viewers (orbital_view.cpp, atom_view.cpp).
inline constexpr int64_t kViewIdleJumpUs = 60'000'000;

// ============================================================================================
// Tumble camera pacing + point rendering (render/camera.h)
// ============================================================================================

inline constexpr orb_real_t kCameraAngleStep = orb_real_t(0.030); ///< Yaw change per frame.
inline constexpr orb_real_t kCameraTiltStep = orb_real_t(0.023);  ///< Tilt change per frame.
inline constexpr orb_real_t kCameraRollStep = orb_real_t(0.017);  ///< Roll change per frame.

/// Tilt/roll start away from the degenerate all-zero pose (where yaw alone can't move
/// axis-aligned lobes at all), so even the first frame after boot isn't axis-locked.
inline constexpr orb_real_t kCameraTiltStart = orb_real_t(0.9);
inline constexpr orb_real_t kCameraRollStart = orb_real_t(2.1);

/// Diameter in pixels of the circular proton marker drawn by drawProtonMarker() (camera.h's
/// shared default; orbital_view.cpp/atom_view.cpp pass their own larger
/// kOrbitalProtonMarkerSize/kAtomProtonMarkerSize below instead).
inline constexpr int kProtonMarkerSize = 3;

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
inline constexpr uint16_t kElectronAlphaQ8 = 240;

/// Fraction of the previous frame buffer's brightness kept each frame (Display::
/// fadeColor565()) instead of hard-clearing it, so points leave a brief trailing glow and
/// skipped "buzz" points fade out instead of vanishing outright. Tuned high enough that a hit
/// pixel's glow stays alive between re-hits as points sweep across the screen during
/// rotation, filling in gaps that would otherwise read as flicker.
inline constexpr uint16_t kPersistenceKeepQ8 = 160; // 160/256 kept per frame (~0.625)

// --- Hidden-points "buzz" flicker (feeds renderPointsColored()'s buzzThreshold param) ---
/// Fraction of points skipped on any given frame (a different pseudo-random subset each
/// frame), so the cloud reads as a buzzing/flickering probability cloud rather than a static
/// fixed set of dots. Shared by orbital_view.cpp and atom_view.cpp so the effect reads the
/// same across both cloud types.
inline constexpr orb_real_t kHiddenPointsFraction = orb_real_t(0.15);
/// Threshold derived from kHiddenPointsFraction for renderPointsColored()'s 16-bit hash. 0
/// (rather than this constant) disables buzz outright, since the hash is unsigned and never
/// compares less than 0.
inline constexpr uint32_t kHiddenPointsThreshold = uint32_t(kHiddenPointsFraction * orb_real_t(65536));

// --- Fly-over easing pace ---
// Camera starts at baseScale * factor and eases to baseScale over `frames` frames. Boot intro
// is slower/more dramatic than a preset/element switch, which is more dramatic than a
// mid-scene zoom excursion.
inline constexpr orb_real_t kIntroStartScaleFactor = orb_real_t(12.0);  ///< Boot intro start-scale multiplier.
inline constexpr int kIntroFrames = 70;                                 ///< Boot intro ease duration, frames.
inline constexpr orb_real_t kSwitchStartScaleFactor = orb_real_t(10.0); ///< Preset/element switch start-scale multiplier.
inline constexpr int kSwitchTransitionFrames = 35;                      ///< Preset/element switch ease duration, frames.

// --- Random zoom excursions (steady-state loop) ---
// At randomized intervals (re-rolled after each one), ease from the current breathing scale
// to a randomized target and back -- layered on top of the constant sine-wave breathing so
// the animation doesn't read as purely mechanical.
inline constexpr int kZoomExcursionMinIntervalFrames = 150;                 ///< Shortest gap between excursions, frames.
inline constexpr int kZoomExcursionMaxIntervalFrames = 500;                 ///< Longest gap between excursions, frames.
inline constexpr orb_real_t kZoomExcursionScaleMinFactor = orb_real_t(0.3); ///< Excursion "zoom out" target multiplier.
/// Excursion "zoom in" target multiplier. Scale multiplies screen-space offset directly
/// (projectPoint()), so a larger factor is the "dive IN" direction: the outer subshell's own
/// radius grows well past the screen edge and only near-center points stay visible, reading
/// as flying through/inside the cloud rather than a simple zoom-in on the whole shape.
/// Matches kIntroStartScaleFactor -- the same "extremely close" magnitude already validated
/// on hardware via the boot intro.
inline constexpr orb_real_t kZoomExcursionScaleMaxFactor = orb_real_t(10.0);
/// Excursion ease duration, frames. A deeper dive (see kZoomExcursionScaleMaxFactor) covers
/// more visual distance per frame, so needs more frames to read as a deliberate dive-through
/// rather than a jarring flash.
inline constexpr int kZoomExcursionEaseFrames = 45;
/// Breathing zoom's angular speed; independent phase from kCameraAngleStep.
inline constexpr orb_real_t kZoomAngleStep = orb_real_t(0.016);

// ============================================================================================
// Overlay text layout (render/overlay.h)
// ============================================================================================

// Matches device_render_common.py's TITLE_TEXT_POS/LOADING_TEXT_POS.
inline constexpr int kTitleTextX = 1, kTitleTextY = 1;
inline constexpr int kLoadingTextX = 2, kLoadingTextY = 22;

// ============================================================================================
// Ticker scroll speed (render/ticker.h)
// ============================================================================================

inline constexpr int kTickerDefaultPxPerFrame = 4;

// ============================================================================================
// Boot splash hold (main.cpp)
// ============================================================================================

/// How long the boot splash (see splash_bitmap.h) is held before IMU/tilt setup starts.
inline constexpr uint32_t kSplashHoldMs = 2000;

// ============================================================================================
// Orbital point-cloud density/turnover (physics/orbital_presets.h)
// ============================================================================================

// Orbital point-cloud size: sizes OrbitalPresetState's point/color/psi2Sorted -- matches
// cloud_common.N_POINTS. That steady-state storage, and the load()-only scratch (orbital_view.
// cpp's psi2/signs/levels, orbital_presets.cpp's order/radii), no longer cost anything of
// their own: they come from physics/view_steady_arena.h's/physics/view_scratch_arena.h's
// shared arenas, each unioned against atom_cloud.cpp's/atom_view.h's equivalent below (never
// needed at the same time, since only one of the two views is ever active) -- see those
// headers for the full rationale.
//
// CYD (plain ESP32, no PSRAM) needs a much smaller count than the S3: every array sized off
// this constant (here, atom_cloud.h's kAtomNumPoints below, and every EXT_RAM_BSS_ATTR buffer
// derived from either -- orbital_view.cpp, atom_view.cpp, benchmark_test.cpp,
// screenshot_batch.cpp, orbital_library.cpp, atom_cloud.cpp, orbital_presets.cpp,
// view_scratch_arena.h, view_steady_arena.h) falls back from PSRAM to internal DRAM with no
// PSRAM to fall back to, and ESP-IDF/PlatformIO link the "main" component whole-archive, so
// even code never called at runtime (e.g. benchmark_test.cpp when BENCHMARK_TEST isn't
// defined) still reserves its own static buffers in that same budget. See CYD-branch.md for
// the measured link-time headroom this value was tuned against.
#if CONFIG_IDF_TARGET_ESP32
// Both runOrbitalView() and runAtomView() (and their static preset state) are always linked
// into the CYD binary regardless of which is on screen at any given moment -- chooser.cpp's
// menu can reach either one -- so both views' static state is permanently resident. Four
// internal-SRAM
// reclamation passes (2026-08-30) made room to raise this back up from an earlier 1800/500
// low point without shrinking the framebuffer or risking boot failure, each verified via a
// real serial boot log's `total free DMA` line (Display::Display(), display.cpp) against the
// 153600 bytes the 240x320 framebuffer needs:
//   1. hfs_radial.cpp's three independent ~4KB RadialTable scratch statics (used
//      mutually-exclusively, one per atom drawing group) merged into one shared pair --
//      ~12.5KB freed on both boards, unconditionally (these were never EXT_RAM_BSS_ATTR/
//      PSRAM-eligible in the first place).
//   2. orbital_presets.cpp's scaleFromRadii() stopped keeping its own separate radii[]
//      scratch, now reuses the same OrderRadiiScratch union computeOrbitalLevels() already
//      used -- ~7.2KB freed at the then-current 1800 points.
//   3. physics/view_scratch_arena.h: orbital_view.cpp's psi2/signs/levels scratch,
//      orbital_presets.cpp's order/radii scratch, and atom_cloud.cpp's sSubshellRadii scratch
//      (kAtomNumPoints below) now share one arena instead of three separate reservations --
//      safe because runOrbitalView()'s and runAtomView()'s scratch needs are never alive at
//      the same time.
//   4. physics/view_steady_arena.h: OrbitalPresetState's points/colors/resample.psi2Sorted and
//      AtomPresetState's points (the actual per-point data, not just scratch) now share ONE
//      arena too, sized to whichever view is larger -- again safe because the two live views
//      never run concurrently. ONLY the two live-view instances (runOrbitalView()'s/
//      runAtomView()'s own `preset` statics) bind to this arena; debug/screenshot_batch.cpp's
//      and debug/gif_capture_test.cpp's own separate preset instances deliberately keep their
//      own private backing storage instead (see view_steady_arena.h's header comment for why
//      -- those can run concurrently with a live view via the screenshot console's task, so
//      sharing there would silently corrupt whatever the live view is showing). Trade-off:
//      since the two live views' points/colors now share physical storage, re-entering one
//      after the other has run must always call load() again (a fresh ~100-200ms rebuild)
//      instead of instantly resuming -- the OLD "presetIndex < 0"/"preset.z == 0"
//      first-boot-only reload guards are gone, replaced with an unconditional reload every
//      re-entry (see runOrbitalView()'s/runAtomView()'s own comments). The rebuilt preset is
//      visually IDENTICAL (same remembered index/Z, same fixed RNG seed), so this only costs
//      the reload pause, never a different result.
// Points 1-3 alone (kOrbitalNumPoints=1800, kAtomNumPoints=3000) measured ~16.2KB free DMA
// margin; adding point 4 (kOrbitalNumPoints still 1800, kAtomNumPoints=5000) measured ~22.6KB
// margin. kOrbitalNumPoints itself was left at 1800 through all four passes since
// kAtomNumPoints was the constant actually worth raising -- but with kAtomNumPoints settled
// at 5000, raising THIS constant too was tried directly on real hardware (not just
// calculated), per this project's own established practice of trusting a measured
// `total free DMA` over a byte-counted estimate:
//   - 5000 (matching kAtomNumPoints): FAILS TO LINK -- `.dram0.bss` overflows dram0_0_seg by
//     2048 bytes. Even if it had linked, this is past where real margin would matter anyway.
//   - 4000: links, but boots to a hard abort() -- `total free DMA=144288` against the 153600
//     the framebuffer needs, i.e. already negative before backoff even starts.
//   - 3500: boots, but the framebuffer allocator backs off all the way to 320 one-row blocks
//     for only ~4.7KB of real margin (`total free DMA=158288`) -- the same fragile pattern
//     rejected earlier for kAtomNumPoints (see that constant's history) rather than accepted
//     just because it happens to boot once.
//   - 3000 (this value): boots cleanly, `total free DMA=166288` against 153600 needed --
//     ~12.7KB margin, framebuffer allocator backing off to 13 reasonably-sized 26-row blocks
//     (same granularity as the kAtomNumPoints=3000/5000 milestones above, and in line with the
//     ~12.5KB margin this project has historically treated as an acceptable floor, CYD-branch.
//     md) -- the value kept. Verified with runOrbitalView()/runAtomView() booting and
//     cross-switching cleanly and repeatedly at this exact pair, no crash or corruption.
inline constexpr int kOrbitalNumPoints = 3000;
#else
inline constexpr int kOrbitalNumPoints = 12000;
#endif

// Point-turnover: fraction of the cloud resampled every kOrbitalCullRefreshFrames frames.
inline constexpr orb_real_t kOrbitalCullFraction = orb_real_t(0.01);
inline constexpr int kOrbitalCullRefreshFrames = 3;
// Per-frame "buzz" flicker fraction lives in kHiddenPointsFraction above, shared with
// atom_view.cpp -- see that constant's comment.

// ============================================================================================
// Atom point-cloud density/shading/scale (physics/atom_cloud.h)
// ============================================================================================

// Atom point-cloud size: sizes AtomPresetState's point array (atom_view.h). Its per-subshell
// radius scratch (outerSubshellRRef()/subshellDissectionPlan(), atom_cloud.cpp) and the point
// array itself no longer cost anything of their own on top of whatever orbital_view.h's own
// arrays already reserve -- see kOrbitalNumPoints's comment above,
// physics/view_scratch_arena.h, and physics/view_steady_arena.h. CYD value: see
// kOrbitalNumPoints's comment above for the four reclamation passes that made this increase
// possible.
#if CONFIG_IDF_TARGET_ESP32
// Raised from 500 (2026-08-30) in two steps as the reclamation passes described in
// kOrbitalNumPoints's comment above landed: first to 3000 (points 1-3, scratch-only sharing),
// then to 5000, this value (point 4, sharing the steady-state points/colors themselves too) --
// an atom point is actually cheaper than an orbital point (12 bytes: just x/y/z, no per-point
// color -- shading is per-subshell via AtomSubshellRange, not per-point like orbital's
// turnover-driven colors[]), so this was never a per-point-cost problem, only a fixed-overhead
// one. Verified via real serial boot logs at both values (see kOrbitalNumPoints's comment for
// the measured DMA margins), and both runOrbitalView() and runAtomView() (including the
// element-load path that exercises the shared sSubshellRadii scratch via
// refreshDissectPlan(), and repeated cross-view switching that exercises the forced reload
// physics/view_steady_arena.h's sharing now requires) booting and running cleanly, repeatedly,
// with no crash or corruption.
inline constexpr int kAtomNumPoints = 5000;
#else
inline constexpr int kAtomNumPoints = 12000;
#endif

// atom_cloud.py's own OUTER_SHELL_BRIGHTEN=0.92 pushes almost to pure white, tuned for a
// renderer that either draws the point outright with no discount (its "device path"
// comment) or discounts it via PC's own ELECTRON_ALPHA blend -- either way, a single hit
// already reads as near-white there. Kept lower here (0.3, closer to that same docstring's
// other quoted figure of 0.55, "read as only a modest change") since this renderer ALSO
// alpha-blends (kElectronAlphaQ8 above) and the valence subshell typically covers a much
// larger on-screen area than the dimmed core -- 0.92 read as "the atom is just white" rather
// than "the valence shell stands out from a colored core" once actually seen on this panel.
inline constexpr orb_real_t kAtomOuterShellBrighten = orb_real_t(0.4); // lerp toward white
inline constexpr orb_real_t kAtomInnerShellDim = orb_real_t(0.2);      // scale toward black

/// Brighten factor for the actively-dissected shell in atom_view.cpp's buildDissectGroups()
/// -- stronger than kAtomOuterShellBrighten above so the one shell currently being examined
/// reads as clearly "lit up" against the flat kDissectDimColor peeled-away shells around it,
/// not just its own plain shell color.
inline constexpr orb_real_t kDissectActiveBrighten = orb_real_t(0.7); // lerp toward white

// --- Scale (M4, revised) ---
//
// Deviation from atom_cloud.py: that model deliberately uses ONE fixed pixels-per-Bohr-
// radius, calibrated against Rubidium (Z=37), so switching elements shows the real
// periodic size trend (noble gases small and tight, alkali metals big and diffuse).
// Abandoned here per explicit user decision after seeing it on this panel: a 240px screen
// with no zoom control yet (that lands in M5) makes most elements read as "just far away"
// under that scheme. Every element now renormalizes to the SAME on-screen target radius
// instead, exactly like orbital_presets.h's scaleFromRadii() already does for hydrogen
// presets -- consistent, always-legible size at the cost of the periodic size trend.
inline constexpr orb_real_t kAtomTargetPx = orb_real_t(Display::kDisplayHeight / 2.5);

inline constexpr orb_real_t kAtomZoomAmplitudeFraction = orb_real_t(0.4); // matches cloud_common.ZOOM_AMPLITUDE_FRACTION

// ============================================================================================
// Tilt arrow geometry (ux/tilt_gesture.h)
// ============================================================================================

/// Tunable geometry for drawTiltArrow()'s triangle.
inline constexpr int kTiltArrowMarginPx = 14;    ///< Gap between the arrow's tip and the screen edge.
inline constexpr int kTiltArrowLengthPx = 24;    ///< Tip-to-base along the pointing direction.
inline constexpr int kTiltArrowHalfWidthPx = 14; ///< Half the base width.

// ============================================================================================
// Chooser menu layout/timing (ux/chooser.cpp)
// ============================================================================================

inline constexpr int kChooserPollDelayMs = 30; ///< Poll/redraw cadence for both the menu and calibration loops.

/// Idle time on the menu (no confirmed tilt-hold) before it auto-launches a demo viewer,
/// coin-flipping between orbitals and elements -- shorter than kViewIdleJumpUs (60s) since the
/// splash is meant to be a passive attract screen, not something visitors are expected to
/// interact with first.
inline constexpr int64_t kChooserIdleJumpUs = 30'000'000;

/// Text scale for the "and hold"/direction-name calibration lines, on top of kFontLarge.
inline constexpr int kCalibLineY0 = 60;      ///< Y of the first calibration line.
inline constexpr int kCalibLineSpacing = 36; ///< Vertical gap between calibration lines -- more than a bare
                                             ///< lineAdvance since these are separate standalone lines, not wrapped body text.

/// Menu option text scale, on top of kFontLarge. 1 (native size) is the largest that keeps
/// the longer "DOWN: Elements" line (186px unscaled) inside the 240px panel width -- scale 2
/// (372px) ran off both edges.
inline constexpr int kChooserOptionScale = 1;

/// Fixed-color toolbar band across the bottom of the splash screen, painted flat every poll
/// (cheap -- a plain rect fill, unlike the ~60-90ms JPEG background decode) so the option
/// text and the tilt-direction cluster below never sit on top of the splash artwork and never
/// need to know or preserve whatever pixels were under them: the whole band is reserved,
/// graphics-free UI chrome, redrawn from scratch each time. This replaces the old per-arrow
/// pixel capture/restore cache (ux/tilt_gesture.cpp's captureArrowBackgrounds()/
/// restoreArrowBackground(), removed) entirely.
inline constexpr int kChooserBandY = 150; ///< Top of the band; spans the full width down to the screen bottom.
inline constexpr uint16_t kChooserBandColor = Display::kColorBlack;

/// Bright, high-contrast colors the menu options blink between (rather than blinking
/// on/off) so they read clearly over the flat band (unlike kColorOrbitalRed, which is
/// tuned for orbital-lobe shading, not on-screen text legibility) and stay flashy even
/// during the "off" half of the cycle instead of disappearing.
inline constexpr uint16_t kChooserOptionColorA = Display::packColor565(255, 255, 0); // yellow
inline constexpr uint16_t kChooserOptionColorB = Display::packColor565(255, 0, 255); // magenta

/// Blink half-period for the menu options (color-swap cadence) -- fast enough to read as an
/// attention-grabbing flash without being so fast it flickers illegibly.
inline constexpr uint32_t kChooserBlinkHalfPeriodMs = 400;

/// Compact Left/Right/Up/Down indicator cluster inside the band, off to the right of the
/// option text (which spans most of the band's width) and well clear of it vertically too --
/// only the currently-held direction is drawn (drawTiltArrowAt(), ux/tilt_gesture.h), same
/// "appears while tilting" feedback the old screen-edge arrow gave, just relocated into the
/// reserved band instead of wherever the splash artwork happened to be.
inline constexpr int kChooserArrowClusterCx = Display::kDisplayWidth - 40;
inline constexpr int kChooserArrowClusterCy = 259;
inline constexpr int kChooserArrowLengthPx = 16;    ///< Tip-to-base along the pointing direction.
inline constexpr int kChooserArrowHalfWidthPx = 10; ///< Half the base width.

// ============================================================================================
// Orbital-view intro/switch pacing (views/orbital_view.cpp)
// ============================================================================================

// --- Orbital-switch quantum-number reveal (scrollOrbitalIntro()) ---
// Shown before switching to a new preset: a pre-rendered equation image (see
// equation_bitmap.h) sits centered as a dim backdrop, with n/l/m revealed progressively on
// top of it, one stage per short real-time hold.
inline constexpr int kOrbitalIntroEqX = (Display::kDisplayWidth - kEquationBitmapWidth) / 2;   ///< Horizontal center for the equation backdrop.
inline constexpr int kOrbitalIntroEqY = (Display::kDisplayHeight - kEquationBitmapHeight) / 2; ///< Vertical center for the equation backdrop.
/// Backdrop color: close to pure white so it reads as "dimmer white" against the foreground
/// text rather than gray that needs squinting to see through the panel/Pepper's-Ghost prism.
inline constexpr uint16_t kOrbitalIntroEqColor = Display::packColor565(210, 210, 220);
/// Text scale for the n/l/m reveal stages. Fixed at one size (rather than per-stage like
/// atom_view.cpp's pickNameScale()) so the reveal doesn't visually jump bigger->smaller
/// between stages; scale 2 is the largest that keeps every stage's text (including
/// two-digit/negative m) within the panel width.
inline constexpr int kOrbitalIntroNumberScale = 2;
/// Y position for the reveal text, below the vertically-centered equation backdrop.
inline constexpr int kOrbitalIntroNumberY = kOrbitalIntroEqY + kEquationBitmapHeight + 20;
/// Hold duration per reveal stage.
inline constexpr uint32_t kOrbitalIntroStageHoldMs = 550;

// Orbital view runs its zoom/switch animations at 1.5x the shared pacing above (atom_view.cpp
// keeps the stock pacing), via these scaled copies rather than editing the shared constants.
inline constexpr int kOrbitalIntroFrames = 105;            ///< kIntroFrames (70) * 1.5.
inline constexpr int kOrbitalSwitchTransitionFrames = 27;  ///< kSwitchTransitionFrames (18) * 1.5.
inline constexpr int kOrbitalZoomExcursionEaseFrames = 68; ///< round(kZoomExcursionEaseFrames (45) * 1.5).
inline constexpr orb_real_t kOrbitalZoomAngleStep = kZoomAngleStep / orb_real_t(1.5);

/// Diameter in pixels of the circular nucleus marker, passed to drawProtonMarker() after each
/// frame's cloud so it redraws bigger/opaque on top. Bigger than the shared kProtonMarkerSize
/// (3px) so the nucleus reads clearly even where orbital density peaks at the origin.
inline constexpr int kOrbitalProtonMarkerSize = 5;

// ============================================================================================
// Atom-view proton/intro/dissection pacing (views/atom_view.cpp)
// ============================================================================================

/// Diameter in pixels of the circular nucleus marker, passed to drawProtonMarker() after each
/// frame's cloud so it redraws bigger/opaque on top.
inline constexpr int kAtomProtonMarkerSize = 5;

/// Neutral outline color for the bounding-sphere silhouette (render/overlay.h's
/// drawBoundingCircle()) -- matches pc/viewer_common.py's/web/py/web_common.py's shared
/// BOUNDING_SPHERE_COLOR, deliberately not shell-colored so it doesn't compete with the cloud.
inline constexpr uint16_t kBoundingCircleColor = Display::packColor565(180, 180, 180);

// --- Element-switch intro ticker (scrollElementIntro()) ---
inline constexpr int kElementIntroMaxNameScale = 4;                                      ///< Largest text scale tried for the sliding element name.
inline constexpr int kElementIntroMinNameScale = 2;                                      ///< Fallback scale if the name doesn't fit at any larger size.
inline constexpr int kElementIntroNameMarginPx = 10;                                     ///< Horizontal margin the name must fit within.
inline constexpr int kElementIntroSymbolScale = 6;                                       ///< Text scale for the pale background symbol watermark.
inline constexpr uint16_t kElementIntroSymbolColor = Display::packColor565(90, 90, 100); ///< Watermark color: dim, so it doesn't compete with the name.
inline constexpr int kElementIntroPxPerFrame = 6;                                        ///< Horizontal slide-in speed of the name, px/frame.
inline constexpr uint32_t kElementIntroHoldMs = 500;                                     ///< Pause once the name reaches center, before flashing.
inline constexpr uint32_t kElementIntroFlashHalfPeriodMs = 1000;                         ///< Flash on/off half-period at center (2 flashes total).

// --- On-device shell dissection (runDissectionSequence() and helpers) ---
inline constexpr uint16_t kDissectDimColor = Display::packColor565(70, 70, 70); ///< Flat shade for shells peeled inward of the active one.
inline constexpr int64_t kDissectHoldUs = 2 * 1000 * 1000;                      ///< Real-time (not frame-count) hold on each revealed shell before advancing.
inline constexpr int kDissectOccMarginPx = 1;                                   ///< Margin for the small electron-count corner note.
inline constexpr orb_real_t kDissectFlySpeedPmPerSec = orb_real_t(30);          ///< Camera-ease speed between shells, picometers per real second.
inline constexpr uint32_t kDissectFlyMinMs = 700;                               ///< Floor on ease duration so a near-zero hop still eases briefly instead of cutting instantly.
/// Real-time hold on the full atom after a dissection sequence finishes (or is cancelled)
/// easing back out, before runAtomView()'s main loop is free to act on another tilt/idle
/// transition -- so a completed dissection never runs straight into another element switch or
/// idle jump with no breathing room in between.
inline constexpr int64_t kDissectPostReturnHoldUs = 1 * 1000 * 1000;

// --- Dissection intro title card (showElectronConfigIntro()) ---
inline constexpr int kDissectIntroMaxWordScale = 2;                                 ///< Largest text scale tried for the 3-line reveal (falls back smaller if a line doesn't fit).
inline constexpr int kDissectIntroMarginPx = 10;                                    ///< Horizontal margin each revealed line must fit within.
inline constexpr int kDissectIntroFirstLineY = 40;                                  ///< Y of the first revealed line.
inline constexpr int kDissectIntroLineGapPx = 10;                                   ///< Extra vertical gap between revealed lines, on top of the scaled line height.
inline constexpr uint32_t kDissectIntroStageHoldMs = 900;                           ///< Hold after each of the first two lines appears.
inline constexpr uint32_t kDissectIntroHoldMs = 3000;                               ///< Total time the card is shown (all 3 stages) before dissection starts.
inline constexpr uint16_t kDissectIntroBgColor = Display::packColor565(55, 55, 55); ///< Dim color for the tiled electron-symbol backdrop.
inline constexpr int kDissectIntroBgSpacingX = 44;                                  ///< Backdrop tile spacing, px.
inline constexpr int kDissectIntroBgSpacingY = 34;
