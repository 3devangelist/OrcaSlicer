#pragma once

// [multi-printer] GUI side of per-plate process settings (fork 3devangelist/OrcaSlicer, see
// fork/DESIGN.md). With the "Process settings per plate" preference on, the plate tab shows every
// process page, a plate can override any process setting, and objects inherit global -> plate ->
// object. The core side (3MF storage) is libslic3r/MultiPrinter/PlateOverrides.

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

class PartPlate;

namespace MultiPrinterGui {

// AppConfig key of the preference. Off by default; read once per run (the tabs are built at start).
inline constexpr const char* PREFERENCE_KEY = "fork_plate_process_settings";

bool plate_process_settings_enabled();

// Keys the plate tab handles: the native plate keys, plus every process setting when enabled.
std::vector<std::string> plate_tab_keys(const std::vector<std::string>& native_keys);

// Stores (or, with `reset`, removes) the plate's override of `key`, taken from the plate tab's config,
// and marks the plate for re-slicing. Native plate keys are left to the plate tab's own code.
void store_plate_value(PartPlate* plate, const DynamicPrintConfig& tab_config, const std::string& key, bool reset);

// Keys an object tab copies from the plate tab before showing the object's own settings: the native
// plate keys, plus, when enabled, the process settings the selected plate overrides.
std::vector<std::string> keys_objects_inherit(const std::vector<std::string>& native_keys,
                                              const DynamicPrintConfig& plate_tab_config,
                                              const DynamicPrintConfig& print_tab_config);

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
