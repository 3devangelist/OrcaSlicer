#pragma once

// [multi-printer] Per-plate process settings (fork 3devangelist/OrcaSlicer, see fork/DESIGN.md).
//
// A plate's config (PartPlate::m_config, PlateData::config) already overrides the global config
// before slicing, and objects override the plate. Upstream only stores a fixed set of "native" plate
// keys there (bed type, print sequence, ...). This module lets any other process (print) setting
// live there too, and stores those extra keys in the plate block of Metadata/model_settings.config as
// <metadata key="plate_override:<opt_key>" value="..."/>. Upstream readers skip unknown plate keys,
// so such a project still opens in upstream OrcaSlicer and Bambu Studio (without the overrides).

#include <ostream>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace MultiPrinter {

// Prefix of the metadata keys that carry per-plate process overrides in a 3MF plate block.
inline constexpr const char* PLATE_OVERRIDE_PREFIX = "plate_override:";

// True for keys the upstream plate code already stores itself (bed type, print sequence, ...).
bool is_native_plate_key(const std::string& opt_key);

// True for a print (process) setting that may be overridden on a plate.
bool is_plate_override_key(const std::string& opt_key);

// The per-plate process overrides held in a plate config, in the config's key order.
std::vector<std::string> plate_override_keys(const DynamicPrintConfig& plate_config);

bool has_plate_overrides(const DynamicPrintConfig& plate_config);

// Writes one <metadata key="plate_override:KEY" value="..."/> line per override, with `indent`
// before each line, matching the surrounding plate block of model_settings.config.
void write_plate_overrides(std::ostream& stream, const DynamicPrintConfig& plate_config, const std::string& indent);

// Handles one metadata entry of a plate block. Returns false when `key` is not an override key, so
// the caller can go on with its own handling. Unknown or invalid settings are skipped (returns true).
bool read_plate_override(DynamicPrintConfig& plate_config, const std::string& key, const std::string& value);

} // namespace MultiPrinter
} // namespace Slic3r
