// [multi-printer] GUI side of per-plate process settings. See PlateSettings.hpp and fork/DESIGN.md.

#include "PlateSettings.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/MultiPrinter/PlateOverrides.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

bool plate_process_settings_enabled()
{
    static const bool enabled = wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool(PREFERENCE_KEY);
    return enabled;
}

std::vector<std::string> plate_tab_keys(const std::vector<std::string>& native_keys)
{
    std::vector<std::string> keys = native_keys;
    if (plate_process_settings_enabled())
        for (const std::string& key : Preset::print_options())
            if (Slic3r::MultiPrinter::is_plate_override_key(key))
                keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

void store_plate_value(PartPlate* plate, const DynamicPrintConfig& tab_config, const std::string& key, bool reset)
{
    if (plate == nullptr || !Slic3r::MultiPrinter::is_plate_override_key(key))
        return;
    DynamicPrintConfig& plate_config = *plate->config();
    if (reset)
        plate_config.erase(key);
    else if (tab_config.has(key))
        plate_config.apply_only(tab_config, {key});
    else
        return;
    plate->update_slice_result_valid_state(false);
    wxGetApp().plater()->schedule_background_process();
}

std::vector<std::string> keys_objects_inherit(const std::vector<std::string>& native_keys,
                                              const DynamicPrintConfig& plate_tab_config,
                                              const DynamicPrintConfig& print_tab_config)
{
    std::vector<std::string> keys = native_keys;
    if (!plate_process_settings_enabled())
        return keys;
    for (const std::string& key : plate_tab_config.keys()) {
        if (!Slic3r::MultiPrinter::is_plate_override_key(key))
            continue;
        const ConfigOption* plate_value = plate_tab_config.option(key);
        const ConfigOption* print_value = print_tab_config.option(key);
        if (print_value == nullptr || !(*plate_value == *print_value))
            keys.push_back(key);
    }
    return keys;
}

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
