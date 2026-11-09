#pragma once

// [multi-printer] Totals of all sliced plates for quotes (fork 3devangelist/OrcaSlicer, see fork/DESIGN.md).
//
// Upstream's "Statistics of All Plates" adds the plates up by filament slot (1, 2, 3...) with the
// first plate's densities. With a printer and filaments per plate, slot 1 of one plate and slot 1 of
// another may be different materials, so this summary adds them up by the real filament (type, colour
// and preset), each plate with its own densities and prices, and adds per-machine totals and the time
// until everything is printed when the machines print at the same time.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class ImGuiWrapper;
class PartPlateList;

namespace MultiPrinterGui {

struct FilamentTotal
{
    std::string name;   // preset name
    std::string type;   // PLA, PETG...
    std::string colour; // #RRGGBB
    double      meters = 0., grams = 0., cost = 0.;
};

struct MachineTotal
{
    std::string name; // the machine, or the printer for plates without one
    int         plates  = 0;
    double      seconds = 0., grams = 0., cost = 0.;
};

struct AllPlatesSummary
{
    bool                       several_sets = false; // plates use different printers, machines or filaments
    std::vector<FilamentTotal> filaments;
    std::vector<MachineTotal>  machines;
    double                     total_seconds = 0., total_grams = 0., total_cost = 0.;
    double                     parallel_seconds = 0.; // the busiest machine
};

AllPlatesSummary all_plates_summary(PartPlateList& plates, size_t time_mode);

// Adds the summary to the "Statistics of All Plates" window when the plates use several sets.
// Returns whether it did (then upstream's per-slot rows are left out, as they would mix materials).
bool render_all_plates_summary(ImGuiWrapper& imgui, size_t time_mode);

// Whether the plates use several printers, machines or filament lists.
bool plates_use_several_sets();

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
