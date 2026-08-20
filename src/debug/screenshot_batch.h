/**
 * @file screenshot_batch.h
 * @brief Renders every orbital preset (orbital_library.h), a curated set of elements, and a
 *        couple of Fe shell-dissection stills into a private off-screen buffer and saves
 *        each as a PNG to the "storage" partition -- mirrors pc/screenshot.py's
 *        --orbitals/--atoms still-image output, for on-device parity with the PC debug
 *        simulator. Triggered by the 'a'/SS_CAP_ALL console command (see
 *        screenshot_console.h); pull the results with `python3 pc/pull_screenshots.py --all`.
 *
 * Every capture function below calls the same render*Frame()/renderAtomDissectFrame()
 * entry points the live views (atom_view.cpp/orbital_view.cpp) draw every frame with,
 * rather than a separate partial re-implementation -- see those functions' own doc
 * comments (atom_view.h/orbital_view.h) for why: it's what keeps these screenshots
 * pixel-identical to what's actually on screen instead of just resembling it.
 *
 * Deliberately renders into its OWN scratch buffer rather than the live Display: this runs
 * from the console's background task, which could otherwise be running at the same time as
 * the main task's chooser/orbital_view/atom_view loop -- fighting over Display's shared frame
 * buffer and present semaphore. Rendering into a private buffer sidesteps that entirely:
 * whatever's live on the physical screen is untouched, and progress shows up only as saved
 * files plus ESP_LOGI lines on the console.
 */
#pragma once

/// Synchronous -- doesn't return until every preset is captured (tens of seconds; each of
/// the ~60 presets rebuilds its point cloud via rejection sampling before it can be
/// rendered/encoded). Progress is logged per preset via ESP_LOGI.
void captureAllPresets();
