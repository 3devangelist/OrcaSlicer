// [multi-printer] One row of plates per printer. See PlateLayout.hpp and fork/DESIGN.md.

#include "PlateLayout.hpp"

#include "PlatePresets.hpp"
#include "PlateSettings.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

namespace {

// Same gap as upstream's grid (LOGICAL_PART_PLATE_GAP in PartPlate.cpp): a fifth of the bed.
constexpr double PLATE_GAP = 1. / 5.;

bool& suspended()
{
    static bool s = false;
    return s;
}

bool& temporary()
{
    static bool t = false;
    return t;
}

PlateGeometry geometry_of(const DynamicPrintConfig& config)
{
    PlateGeometry g;
    if (const auto* o = config.option<ConfigOptionPoints>("printable_area"))
        g.shape = make_counter_clockwise(o->values); // as Plater::set_bed_shape passes it
    if (const auto* o = config.option<ConfigOptionPoints>("bed_exclude_area"))
        g.exclude_area = o->values;
    if (const auto* o = config.option<ConfigOptionPointsGroups>("extruder_printable_area"))
        g.extruder_areas = o->values;
    if (const auto* o = config.option<ConfigOptionFloatsNullable>("extruder_printable_height"))
        g.extruder_heights = o->values;
    if (const auto* o = config.option<ConfigOptionFloat>("printable_height"))
        g.height = o->value;
    if (!g.shape.empty()) {
        const BoundingBoxf box = get_extents(g.shape);
        g.width = int(box.size().x());
        g.depth = int(box.size().y());
    }
    return g;
}

struct Entry
{
    const PartPlate* plate = nullptr; // nullptr: the plate about to be added
    std::string      printer;
    PlateGeometry    geometry;
};

// The plates in index order, plus the plate about to be added when `with_next`.
std::vector<Entry> entries(PartPlateList& plates, bool with_next)
{
    std::vector<Entry> out;
    const int count = plates.get_plate_count();
    for (int i = 0; i <= count; ++i) {
        if (i == count && !with_next)
            break;
        Entry e;
        e.plate = i < count ? plates.get_plate(i) : nullptr;
        // A plate about to be added copies the current plate, whose set is the one loaded.
        e.geometry = geometry_of(plate_printer_config(e.plate, e.printer));
        // Owner's decision: one row per machine; plates without one go by printer.
        if (const MachineBinding machine = plate_machine(e.plate); !machine.empty())
            e.printer = "machine:" + machine.id;
        out.push_back(std::move(e));
    }
    return out;
}

bool can_lay_out()
{
    return !suspended() && plate_process_settings_enabled() && wxGetApp().plater() != nullptr &&
           wxGetApp().preset_bundle != nullptr;
}

bool uses_several_printers(const std::vector<Entry>& plates)
{
    for (const Entry& e : plates)
        if (e.plate != nullptr && e.printer != plates.front().printer)
            return true;
    return false;
}

struct Layout
{
    std::vector<Vec2d> positions; // per entry
    double             bottom = 0.;
};

// One row per printer, in the order of their first plate; rows go down (-Y) like upstream's grid rows.
Layout lay_out(const std::vector<Entry>& plates)
{
    std::vector<std::string> rows;
    std::vector<double>      row_depth;
    std::vector<size_t>      row_of(plates.size());
    for (size_t i = 0; i < plates.size(); ++i) {
        auto it = std::find(rows.begin(), rows.end(), plates[i].printer);
        if (it == rows.end()) {
            rows.push_back(plates[i].printer);
            row_depth.push_back(0.);
            it = rows.end() - 1;
        }
        row_of[i]              = size_t(it - rows.begin());
        row_depth[row_of[i]]   = std::max(row_depth[row_of[i]], double(plates[i].geometry.depth));
    }
    // A plate spans [origin, origin + depth] in Y. Row r ends a gap (a fifth of the row above) under the
    // row above; with equal beds this is upstream's -row * depth * (1 + gap).
    std::vector<double> row_y(rows.size(), 0.), row_x(rows.size(), 0.);
    for (size_t r = 1; r < rows.size(); ++r)
        row_y[r] = row_y[r - 1] - row_depth[r - 1] * PLATE_GAP - row_depth[r];
    Layout layout;
    const double last_depth = row_depth.empty() ? 0. : row_depth.back();
    const double deepest    = row_depth.empty() ? 0. : *std::max_element(row_depth.begin(), row_depth.end());
    layout.bottom = (rows.empty() ? 0. : row_y.back()) - last_depth * PLATE_GAP - deepest; // room for any bed
    for (size_t i = 0; i < plates.size(); ++i) {
        const size_t r = row_of[i];
        layout.positions.emplace_back(row_x[r], row_y[r]);
        row_x[r] += plates[i].geometry.width * (1. + PLATE_GAP);
    }
    return layout;
}

} // namespace

bool layout_active(PartPlateList& plates)
{
    return can_lay_out() && plates.get_plate_count() > 1 && uses_several_printers(entries(plates, false));
}

std::optional<Vec2d> layout_position(PartPlateList& plates, int index)
{
    if (!can_lay_out() || index < 0 || index > plates.get_plate_count())
        return std::nullopt;
    const std::vector<Entry> all = entries(plates, index == plates.get_plate_count());
    if (!uses_several_printers(all))
        return std::nullopt;
    return lay_out(all).positions[size_t(index)];
}

std::optional<Vec2d> layout_unprintable_position(PartPlateList& plates)
{
    if (!can_lay_out())
        return std::nullopt;
    const std::vector<Entry> all = entries(plates, false);
    if (all.empty() || !uses_several_printers(all))
        return std::nullopt;
    return Vec2d(0., lay_out(all).bottom);
}

std::optional<PlateGeometry> layout_geometry(PartPlateList& plates, int index)
{
    if (!can_lay_out() || index < 0 || index > plates.get_plate_count())
        return std::nullopt;
    const std::vector<Entry> all = entries(plates, index == plates.get_plate_count());
    if (!uses_several_printers(all) || all[size_t(index)].geometry.shape.empty())
        return std::nullopt;
    return all[size_t(index)].geometry;
}

bool layout_changed(PartPlateList& plates)
{
    static std::string last;
    std::ostringstream signature;
    if (layout_active(plates))
        for (const Entry& e : entries(plates, false))
            signature << e.plate << '|' << e.printer << '|' << e.geometry.width << 'x' << e.geometry.depth << ';';
    const bool changed = signature.str() != last;
    last               = signature.str();
    return changed;
}

void suspend_plate_layout(Plater& plater, bool suspend)
{
    PartPlateList& plates = plater.get_partplate_list();
    if (suspend) {
        if (!layout_active(plates))
            return;
        // The file gets upstream's grid, sized for the first plate's printer: the project's printer, which
        // is what upstream OrcaSlicer opens it with.
        std::string               name;
        const DynamicPrintConfig& first    = plate_printer_config(plates.get_plate(0), name);
        const PlateGeometry       geometry = geometry_of(first);
        const auto*               wrapping = first.option<ConfigOptionPoints>("wrapping_exclude_area");
        suspended()                        = true;
        temporary()                        = true;
        plater.set_bed_shape(geometry.shape, geometry.exclude_area, wrapping ? wrapping->values : Pointfs(), geometry.height,
                             geometry.extruder_areas, geometry.extruder_heights, "", "");
        temporary() = false;
    } else if (suspended()) {
        suspended() = false;
        temporary() = true;
        plater.set_bed_shape(); // back to the rows, sized from the current plate's printer
        temporary() = false;
    }
}

bool layout_is_temporary() { return temporary(); }

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
