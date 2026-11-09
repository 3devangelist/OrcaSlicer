#pragma once

// [multi-printer] The 3MF file that carries the printer, process and filaments of plates that have their
// own (fork 3devangelist/OrcaSlicer, see fork/DESIGN.md). Its content is opaque here: the GUI writes and
// reads it (slic3r/GUI/MultiPrinter/PlatePresets). Upstream readers skip unknown Metadata files.

#include <miniz.h>

#include <string>

namespace Slic3r {

class Model;

namespace MultiPrinter {

inline constexpr const char* PLATE_PRESETS_FILE = "Metadata/multi_printer_plates.json";

// Adds the file when the model carries plate presets. Returns false only when writing failed.
bool add_plate_presets_file(mz_zip_archive& archive, const Model& model);

// Reads the file into model.multi_printer_plates.
void read_plate_presets_file(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model);

} // namespace MultiPrinter
} // namespace Slic3r
