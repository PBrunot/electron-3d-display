// On-screen text overlays: title and the physical-size scale bar (FPS is serial-logged
// only, not drawn -- see the ESP_LOGI calls in atom_view.cpp/orbital_view.cpp). Port of
// micropython/cloud_common.py's SCALE_BAR_CANDIDATES/pick_scale_bar_length() +
// micropython/device_render_common.py's draw_scale_bar(), on top of font.h's bitmap text
// instead of framebuf. Panel-native coordinates, NOT prism-mirror-corrected -- same "not
// worth the effort for dev/debug text" call device_render_common.py already makes (see
// camera.h's header comment on this unit's still-open ESP-IDF geometry question).
#pragma once

#include <cstdint>

class Display;

#include "physics/orbitals.h" // orb_real_t
#include "config/visual_constants.h" // kTitleTextX/Y, kLoadingTextX/Y

// 1 Bohr radius in picometers (CODATA a0 = 0.52917721090(80)e-10 m), matches
// cloud_common.PM_PER_BOHR / atom_cloud.PM_PER_BOHR -- both MicroPython modules already
// re-export the same constant under their own name; this is the single C++ source of
// truth for both viewers' scale bars.
inline constexpr orb_real_t kPmPerBohr = orb_real_t(52.9177210903);

/**
 * Bottom-left physical-size reference bar. `pixelsPerUnit` is screen pixels per physical
 * unit at the CURRENT zoom (e.g. scale / kPmPerBohr for a pm-labeled bar); pixelsPerUnit
 * <= 0 draws nothing (defensive only, matches device_render_common.draw_scale_bar()).
 * Picks the largest "nice round length" (1/2/5 x a power of ten, see overlay.cpp's
 * kScaleBarCandidates) that still fits under the bar's max on-screen length.
 */
void drawScaleBar(Display &display, orb_real_t pixelsPerUnit, const char *unitLabel, uint16_t barColor,
                  uint16_t textColor);

/**
 * @brief Screen-centered outline circle of radius rRef*scale -- the atom's bounding-sphere
 * silhouette, drawn on top of the point cloud so its outer edge reads clearly even where the
 * cloud itself thins out toward the boundary. Port of pc/viewer_common.py's
 * draw_bounding_circle() / web/py/web_common.py's draw_bounding_circle_canvas(); rRef <= 0
 * draws nothing.
 */
void drawBoundingCircle(Display &display, orb_real_t rRef, orb_real_t scale, uint16_t color);

inline constexpr const char *kLoadingText = "Loading...";
