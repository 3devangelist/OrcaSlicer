// [multi-printer] See PlatePresetsFile.hpp.

#include "PlatePresetsFile.hpp"

#include "libslic3r/Model.hpp"

#include <boost/log/trivial.hpp>

#include <string>

namespace Slic3r {
namespace MultiPrinter {

bool add_plate_presets_file(mz_zip_archive& archive, const Model& model)
{
    if (model.multi_printer_plates.empty())
        return true;
    if (!mz_zip_writer_add_mem(&archive, PLATE_PRESETS_FILE, model.multi_printer_plates.data(),
                               model.multi_printer_plates.size(), MZ_DEFAULT_COMPRESSION)) {
        BOOST_LOG_TRIVIAL(error) << "3MF: unable to add " << PLATE_PRESETS_FILE;
        return false;
    }
    return true;
}

void read_plate_presets_file(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model)
{
    constexpr mz_uint64 max_size = mz_uint64(256) << 20; // 256 MiB: far above any real set of presets
    if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > max_size) {
        BOOST_LOG_TRIVIAL(error) << "3MF: " << PLATE_PRESETS_FILE << " of " << stat.m_uncomp_size << " bytes not loaded";
        return;
    }
    std::string buffer(size_t(stat.m_uncomp_size), '\0');
    if (mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, buffer.data(), buffer.size(), 0))
        model.multi_printer_plates = std::move(buffer);
}

} // namespace MultiPrinter
} // namespace Slic3r
