// Decodes img/orbitals.jpg (mirrored at data/orbitals.jpg) on-device via jpeg_bg.h -- backdrop
// for OrbitalView's quantum-number reveal (views/orbital_view.cpp's scrollOrbitalIntro()).
#pragma once

class Display;

inline constexpr int kOrbitalsBitmapWidth = 240;
inline constexpr int kOrbitalsBitmapHeight = 240;

/// Blits data/orbitals.jpg centered on the display, opaque -- see jpeg_bg.h.
void drawOrbitalsBackdrop(Display &display);
