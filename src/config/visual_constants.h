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

// Orbital point-cloud size: sizes OrbitalPresetState's point/color arrays (orbital_view.h)
// and the scratch arrays orbital_presets.cpp's computeOrbitalLevels()/scaleFromRadii() use
// (order[], radii[]), plus OrbitalResampleState::psi2Sorted -- matches cloud_common.N_POINTS.
//
// CYD (plain ESP32, no PSRAM) needs a much smaller count than the S3: every array sized off
// this constant (here, atom_cloud.h's kAtomNumPoints below, and every EXT_RAM_BSS_ATTR buffer
// derived from either -- orbital_view.cpp, atom_view.cpp, orbital_slice.cpp, benchmark_test.cpp,
// screenshot_batch.cpp, orbital_slice_test.cpp, orbital_library.cpp, atom_cloud.cpp,
// orbital_presets.cpp) falls back from PSRAM to internal DRAM with no PSRAM to fall back to,
// and ESP-IDF/PlatformIO link the "main" component whole-archive, so even code never called at
// runtime (e.g. benchmark_test.cpp when BENCHMARK_TEST isn't defined) still reserves its own
// static buffers in that same budget. See CYD-branch.md for the measured link-time headroom
// this value was tuned against.
#if CONFIG_IDF_TARGET_ESP32
inline constexpr int kOrbitalNumPoints = 2000;
#else
inline constexpr int kOrbitalNumPoints = 12000;
#endif

// Point-turnover: fraction of the cloud resampled every kOrbitalCullRefreshFrames frames.
inline constexpr orb_real_t kOrbitalCullFraction = orb_real_t(0.01);
inline constexpr int kOrbitalCullRefreshFrames = 3;
// Per-frame "buzz" flicker fraction lives in kHiddenPointsFraction above, shared with
// atom_view.cpp -- see that constant's comment.

// ============================================================================================
// Orbital slice view (views/orbital_slice.h, runSliceSequence() in views/orbital_view.cpp)
// ============================================================================================

/// Heatmap grid resolution: kSliceGridSize x kSliceGridSize cells, one per screen pixel --
/// full panel resolution (240 x 240) so the heatmap is smooth, imshow-style, with no block
/// artifacts (the earlier 2x2-cell version looked blocky). kSliceCellPx stays derived (== 1).
inline constexpr int kSliceGridSize = Display::kDisplayWidth;
inline constexpr int kSliceCellPx = Display::kDisplayWidth / kSliceGridSize;

/// Grid half-extent as a multiple of the preset's own p90 reference radius (rRef) -- >1 so
/// the outer lobes have a little margin instead of touching the panel edge.
inline constexpr orb_real_t kSliceFramingFactor = orb_real_t(1.2);

/// Real-time hold on the finished heatmap before auto-fading back to the 3D view (D3, see
/// SLICE.md) -- any tilt movement aborts sooner. Same real-time-not-frame-count convention as
/// atom_view.cpp's kDissectHoldUs.
inline constexpr int64_t kSliceHoldUs = 12 * 1000 * 1000;

/// How long the static "Sezione" intro card (D4) holds before the heatmap build starts.
inline constexpr uint32_t kSliceIntroHoldMs = 900;

/// Fade-in/fade-out duration, frames -- the plane itself is static (D2); this is the only
/// motion in the whole sequence.
inline constexpr int kSliceIntroFrames = 30;
inline constexpr int kSliceFadeOutFrames = 20;

/// Brightness mapping for the slice heatmap (orbital_slice.cpp's buildSliceTable()):
/// level = 255 * min(1, |psi|^2 / v99)^gamma, with v99 = the grid's own
/// kSliceDensityPercentile-th percentile of |psi|^2. Deliberately NOT the 3D cloud's rank
/// curve (orbitalLevelFromRankFraction): a uniform grid has no empty screen space, so
/// rank-equalizing it lights half the screen to ~83% brightness by construction (median level
/// 212 for every orbital, 0% dark cells -- see SLICE.md's contrast note); density
/// normalization preserves the real falloff instead (bright lobe cores, near-black tails).
inline constexpr orb_real_t kSliceDensityPercentile = orb_real_t(0.999);

/// Auto-exposure, per orbital -- mirrors the reference repo's (ssebastianmag/
/// hydrogen-wavefunctions, hwf_plots.py) hand-tuned exposure knob: gamma =
/// max(kSliceMinGamma, 1/(1+exposure)), vmax = the 99.9th percentile of |psi|^2. There, a
/// human picked `exposure` per image by eye (0 for orbitals that already filled the frame,
/// up to 1.5 for tightly-peaked ones) because a flat gamma washes out/saturates the
/// already-bright ones. We compute the equivalent automatically per orbital instead of a
/// hand-picked table: our real orbitals have genuine lobe structure even for m != 0
/// (SLICE.md's phi-dependence note), unlike the reference's azimuthally-uniform complex
/// harmonics, so its per-(n,l,m) choices don't transfer over.
///
/// brightFraction = (cells with density >= kSliceBrightFrac*v99) /
///                  (cells with density >= kSliceVisibleFrac*v99)
/// i.e. what fraction of the visible cloud (density above a small v99-relative floor) is
/// already near-max. A concentrated orbital (small hot core, most of the visible cloud far
/// dimmer than its own peak) gets a low brightFraction -> exposure lift; a broad orbital
/// (density already near v99 across much of its visible footprint) gets a high
/// brightFraction -> little to no lift, avoiding the saturation a flat gamma caused.
inline constexpr orb_real_t kSliceVisibleFrac = orb_real_t(0.01);
inline constexpr orb_real_t kSliceBrightFrac = orb_real_t(0.5);
inline constexpr orb_real_t kSliceExposureScale = orb_real_t(1.5);
inline constexpr orb_real_t kSliceMinGamma = orb_real_t(0.10);

// ============================================================================================
// Atom point-cloud density/shading/scale (physics/atom_cloud.h)
// ============================================================================================

// Atom point-cloud size: sizes AtomPresetState's point array (atom_view.h) and
// outerSubshellRRef()'s per-subshell radius scratch (atom_cloud.cpp). CYD value: see
// kOrbitalNumPoints's comment above -- same no-PSRAM constraint.
#if CONFIG_IDF_TARGET_ESP32
inline constexpr int kAtomNumPoints = 1000;
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
/// Both lines sit in the lower part of the panel (below the splash's own focal art, which
/// occupies the upper/middle area) rather than the previous vertically-centered placement.
inline constexpr int kChooserOption1Y = 180; ///< Y of the "UP: Orbitals" line.
inline constexpr int kChooserOption2Y = 205; ///< Y of the "DOWN: Elements" line.

/// Bright, high-contrast colors the menu options blink between (rather than blinking
/// on/off) so they read clearly over the splash backdrop (unlike kColorOrbitalRed, which is
/// tuned for orbital-lobe shading, not on-screen text legibility) and stay flashy even
/// during the "off" half of the cycle instead of disappearing.
inline constexpr uint16_t kChooserOptionColorA = Display::packColor565(255, 255, 0); // yellow
inline constexpr uint16_t kChooserOptionColorB = Display::packColor565(255, 0, 255); // magenta

/// Blink half-period for the menu options (color-swap cadence) -- fast enough to read as an
/// attention-grabbing flash without being so fast it flickers illegibly.
inline constexpr uint32_t kChooserBlinkHalfPeriodMs = 400;

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

// --- Dissection intro title card (showElectronConfigIntro()) ---
inline constexpr int kDissectIntroWordScale = 2;                                    ///< Text scale for "Configurazione" / "elettronica" / the element name.
inline constexpr int kDissectIntroLineGapPx = 50;                                   ///< Vertical start-to-start spacing between the 3 title lines.
inline constexpr uint32_t kDissectIntroHoldMs = 900;                                ///< How long the static title card holds before dissection starts.
inline constexpr uint16_t kDissectIntroBgColor = Display::packColor565(55, 55, 55); ///< Dim color for the tiled electron-symbol backdrop.
inline constexpr int kDissectIntroBgSpacingX = 44;                                  ///< Backdrop tile spacing, px.
inline constexpr int kDissectIntroBgSpacingY = 34;
