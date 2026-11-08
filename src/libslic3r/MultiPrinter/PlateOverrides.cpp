// [multi-printer] Per-plate process settings. See PlateOverrides.hpp and fork/DESIGN.md.

#include "PlateOverrides.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cstring>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace Slic3r {
namespace MultiPrinter {

bool is_native_plate_key(const std::string& opt_key)
{
    // Keys that PartPlate and the 3MF plate block (bbs_3mf.cpp) handle themselves. Keep in sync with
    // the plate block writer, so no key is stored twice.
    static const std::unordered_set<std::string> native = {
        "curr_bed_type",
        "print_sequence",
        "first_layer_print_sequence",
        "other_layers_print_sequence",
        "other_layers_print_sequence_nums",
        "first_layer_sequence_choice",
        "other_layers_sequence_choice",
        "spiral_mode",
        "imex_parallel_mode",
        "imex_head_filament_map",
        "filament_map_mode",
        "filament_map",
        "filament_volume_map",
        "filament_nozzle_map",
    };
    return native.count(opt_key) != 0;
}

bool is_plate_override_key(const std::string& opt_key)
{
    if (is_native_plate_key(opt_key) || print_config_def.get(opt_key) == nullptr)
        return false;
    static const std::unordered_set<std::string> print_keys(Preset::print_options().begin(), Preset::print_options().end());
    return print_keys.count(opt_key) != 0;
}

std::vector<std::string> plate_override_keys(const DynamicPrintConfig& plate_config)
{
    std::vector<std::string> keys;
    for (const std::string& key : plate_config.keys())
        if (is_plate_override_key(key))
            keys.push_back(key);
    return keys;
}

bool has_plate_overrides(const DynamicPrintConfig& plate_config)
{
    const std::vector<std::string> all = plate_config.keys();
    return std::any_of(all.begin(), all.end(), is_plate_override_key);
}

void write_plate_overrides(std::ostream& stream, const DynamicPrintConfig& plate_config, const std::string& indent)
{
    for (const std::string& key : plate_override_keys(plate_config)) {
        // Config serialization is C-style, not XML: escape it as the volume settings are escaped.
        stream << indent << "<metadata key=\"" << PLATE_OVERRIDE_PREFIX << key << "\" value=\""
               << xml_escape_double_quotes_attribute_value(plate_config.opt_serialize(key)) << "\"/>\n";
    }
}

bool read_plate_override(DynamicPrintConfig& plate_config, const std::string& key, const std::string& value)
{
    const size_t prefix_len = std::strlen(PLATE_OVERRIDE_PREFIX);
    if (key.compare(0, prefix_len, PLATE_OVERRIDE_PREFIX) != 0)
        return false;
    const std::string opt_key = key.substr(prefix_len);
    if (!is_plate_override_key(opt_key))
        return true; // a setting this build does not know or allow on plates: skip it
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    try {
        plate_config.set_deserialize(opt_key, value, substitutions);
    } catch (const std::exception&) {
        // A malformed value must not abort loading the project; the plate keeps the global value.
    }
    return true;
}

} // namespace MultiPrinter
} // namespace Slic3r
