#pragma once

// [multi-printer] Plates of different printers shown at the same time (fork 3devangelist/OrcaSlicer,
// see fork/DESIGN.md).
//
// Owner's decision (2026-11-09): one row per printer, like PrusaSlicer 3.0 lays out beds with different
// configs (Biz/Scene/BedPlacement.cpp there). Plates that use the same printer are side by side in one
// row; each row is as deep as its bed, and each plate has its own bed shape and size. When every plate
// uses the same printer, upstream's square grid is kept, so nothing changes for one-printer projects.
//
// PartPlateList asks this module for each plate's position (compute_shape_position) and bed
// (set_shapes, create_plate, update_all_plates_pos_and_size); upstream code does the rest.

#include "libslic3r/Point.hpp"

#include <optional>
#include <vector>

namespace Slic3r {
namespace GUI {

class PartPlateList;

namespace MultiPrinterGui {

// A plate's own bed, as PartPlate::set_shape and set_pos_and_size take it (shape not yet moved to the
// plate's position).
struct PlateGeometry
{
    Pointfs              shape;
    Pointfs              exclude_area;
    std::vector<Pointfs> extruder_areas;
    std::vector<double>  extruder_heights;
    int                  width  = 0;
    int                  depth  = 0;
    double               height = 0.;
};

// True when the feature is on and the plates use two or more printers.
bool layout_active(PartPlateList& plates);

// Where plate `index` goes. `index` may be the plate count: a plate about to be added from the current
// plate (same printer). Empty when the layout is not active (upstream's grid applies).
std::optional<Vec2d> layout_position(PartPlateList& plates, int index);

// Where the area for objects outside the plates goes: under the last row.
std::optional<Vec2d> layout_unprintable_position(PartPlateList& plates);

// The bed of plate `index` (same rule for `index` as layout_position).
std::optional<PlateGeometry> layout_geometry(PartPlateList& plates, int index);

// True once each time the plates' printers, beds or order changed since the previous call.
bool layout_changed(PartPlateList& plates);

// True while the plates are laid out on upstream's grid for a file and back (suspend_plate_layout):
// a temporary move, so nothing that belongs to the plates (wipe tower positions) may be reset.
bool layout_is_temporary();

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
