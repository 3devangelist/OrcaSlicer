// [multi-printer] Totals of all sliced plates for quotes. See PlateSummary.hpp.

#include "PlateSummary.hpp"

#include "PlatePresets.hpp"
#include "PlateSettings.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

namespace {

std::string string_at(const DynamicPrintConfig& config, const char* key, size_t i)
{
    const auto* o = config.option<ConfigOptionStrings>(key);
    return o != nullptr && !o->values.empty() ? o->values[std::min(i, o->values.size() - 1)] : std::string();
}

double float_at(const DynamicPrintConfig& config, const char* key, size_t i, double fallback)
{
    const auto* o = config.option<ConfigOptionFloats>(key);
    return o != nullptr && !o->values.empty() ? o->values[std::min(i, o->values.size() - 1)] : fallback;
}

// A plate's sliced Print, or nullptr.
const Print* sliced_print(PartPlate* plate)
{
    if (plate == nullptr || plate->empty() || !plate->is_slice_result_valid())
        return nullptr;
    PrintBase* print = nullptr;
    plate->get_print(&print, nullptr, nullptr);
    return dynamic_cast<const Print*>(print);
}

std::string set_key(PartPlate* plate)
{
    std::string printer;
    plate_printer_config(plate, printer);
    const MachineBinding machine = plate_machine(plate);
    return machine.empty() ? printer : machine.id;
}

} // namespace

bool plates_use_several_sets()
{
    if (!plate_process_settings_enabled() || wxGetApp().plater() == nullptr)
        return false;
    PartPlateList&        plates = wxGetApp().plater()->get_partplate_list();
    std::set<std::string> keys;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        const Print* print = sliced_print(plates.get_plate(i));
        if (print == nullptr)
            continue;
        std::string filaments;
        if (const auto* o = print->full_print_config().option<ConfigOptionStrings>("filament_settings_id"))
            for (const std::string& f : o->values)
                filaments += f + ";";
        keys.insert(set_key(plates.get_plate(i)) + "|" + filaments);
    }
    return keys.size() > 1;
}

AllPlatesSummary all_plates_summary(PartPlateList& plates, size_t time_mode)
{
    AllPlatesSummary summary;
    summary.several_sets = plates_use_several_sets();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate*   plate = plates.get_plate(i);
        const Print* print = sliced_print(plate);
        if (print == nullptr || plate->get_slice_result() == nullptr)
            continue;
        const PrintEstimatedStatistics& stats  = plate->get_slice_result()->print_statistics;
        const DynamicPrintConfig&       config = print->full_print_config();
        // Every use of each slot: model, support, flush and wipe tower.
        std::map<size_t, double> volume_per_slot;
        for (const auto* used : {&stats.model_volumes_per_extruder, &stats.support_volumes_per_extruder, &stats.flush_per_filament,
                                 &stats.wipe_tower_volumes_per_extruder})
            for (const auto& [slot, volume] : *used)
                volume_per_slot[slot] += volume;
        double plate_grams = 0., plate_cost = 0.;
        for (const auto& [slot, volume] : volume_per_slot) {
            const double diameter = float_at(config, "filament_diameter", slot, 1.75);
            const double density  = float_at(config, "filament_density", slot, 1.24);
            const double price    = float_at(config, "filament_cost", slot, 0.);
            const double meters   = volume / (M_PI * 0.25 * diameter * diameter) / 1000.;
            const double grams    = volume * density / 1000.;
            const double cost     = grams / 1000. * price;
            const std::string name   = string_at(config, "filament_settings_id", slot);
            const std::string colour = string_at(config, "filament_colour", slot);
            auto it = std::find_if(summary.filaments.begin(), summary.filaments.end(),
                                   [&](const FilamentTotal& f) { return f.name == name && f.colour == colour; });
            if (it == summary.filaments.end())
                it = summary.filaments.insert(summary.filaments.end(), {name, string_at(config, "filament_type", slot), colour});
            it->meters += meters;
            it->grams += grams;
            it->cost += cost;
            plate_grams += grams;
            plate_cost += cost;
        }
        const double seconds = time_mode < stats.modes.size() ? stats.modes[time_mode].time : 0.;
        std::string  label   = plate_machine_name(plate);
        if (label.empty())
            plate_printer_config(plate, label);
        auto m = std::find_if(summary.machines.begin(), summary.machines.end(), [&](const MachineTotal& t) { return t.name == label; });
        if (m == summary.machines.end())
            m = summary.machines.insert(summary.machines.end(), {label});
        m->plates += 1;
        m->seconds += seconds;
        m->grams += plate_grams;
        m->cost += plate_cost;
        summary.total_seconds += seconds;
        summary.total_grams += plate_grams;
        summary.total_cost += plate_cost;
    }
    for (const MachineTotal& m : summary.machines)
        summary.parallel_seconds = std::max(summary.parallel_seconds, m.seconds);
    return summary;
}

bool render_all_plates_summary(ImGuiWrapper& imgui, size_t time_mode)
{
    if (!plates_use_several_sets())
        return false;
    const AllPlatesSummary s = all_plates_summary(wxGetApp().plater()->get_partplate_list(), time_mode);
    char buf[256];
    auto time_text = [](double seconds) { return short_time(get_time_dhms(float(seconds))); };

    ImGui::Separator();
    imgui.title(_u8L("By filament"));
    for (const FilamentTotal& f : s.filaments) {
        ColorRGBA colour;
        if (decode_color(f.colour, colour)) {
            ImGui::ColorButton(("##" + f.name + f.colour).c_str(), ImVec4(colour.r(), colour.g(), colour.b(), 1.f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
            ImGui::SameLine();
        }
        ::snprintf(buf, sizeof(buf), "%s (%s): %.2f m  %.2f g  %.2f", f.name.c_str(), f.type.c_str(), f.meters, f.grams, f.cost);
        imgui.text(buf);
    }
    ImGui::Separator();
    imgui.title(_u8L("By machine"));
    for (const MachineTotal& m : s.machines) {
        ::snprintf(buf, sizeof(buf), "%s: %d %s, %s, %.2f g, %.2f", m.name.c_str(), m.plates, _u8L("plates").c_str(),
                   time_text(m.seconds).c_str(), m.grams, m.cost);
        imgui.text(buf);
    }
    ImGui::Separator();
    imgui.text(_u8L("Machine time (all plates)") + ": " + time_text(s.total_seconds));
    imgui.text(_u8L("Done in (machines at the same time)") + ": " + time_text(s.parallel_seconds));
    ::snprintf(buf, sizeof(buf), "%.2f g, %.2f", s.total_grams, s.total_cost);
    imgui.text(_u8L("Filament and cost (all plates)") + ": " + buf);
    return true;
}

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
