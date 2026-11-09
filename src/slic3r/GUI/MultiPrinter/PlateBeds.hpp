#pragma once

// [multi-printer] Each plate shows the bed model (STL) of its own printer (fork 3devangelist/OrcaSlicer,
// see fork/DESIGN.md).
//
// Upstream draws one bed model (Bed3D), the selected printer's, under the current plate only, and reloads
// it from disk whenever the selected printer changes. With a set per plate that means every plate switch
// reloads an STL and the other plates show no bed. Owner's request (2026-11-09): the bed models stay
// loaded and every plate shows its printer's. While the feature is on, this module draws them for all
// plates from a cache, and Bed3D leaves its own model out (Bed3D::set_model_drawn_elsewhere).

#include "libslic3r/Point.hpp"

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

// True when the plates draw their own bed models (the feature is on).
bool plate_bed_models_enabled();

// Draws the bed model of every plate (only the current plate's when `only_current`), as Bed3D draws its
// own: not from below.
void render_plate_bed_models(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current);

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
