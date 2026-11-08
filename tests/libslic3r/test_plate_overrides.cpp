// [multi-printer] Per-plate process settings (fork 3devangelist/OrcaSlicer, see fork/DESIGN.md).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_utils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiPrinter/PlateOverrides.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <miniz.h>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

struct Project {
    ScopedTemporaryDir backup{"orca_plate_overrides"};
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateDataPtrs plates;

    Project()
    {
        model.set_backup_path(backup.string());
        ModelObject* object = model.add_object();
        object->name = "cube";
        object->add_volume(make_cube(20, 20, 20));
        object->add_instance();
        auto* plate = new PlateData();
        plate->plate_index = 0;
        plate->objects_and_instances.emplace_back(0, 0);
        plates.push_back(plate);
    }
    ~Project() { release_PlateData_list(plates); }

    DynamicPrintConfig& plate_config() { return plates.front()->config; }

    void save(const std::string& path)
    {
        StoreParams params;
        params.path = path;
        params.model = &model;
        params.config = &config;
        params.plate_data_list = plates;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        REQUIRE(store_bbs_3mf(params));
    }
};

struct Loaded {
    ScopedTemporaryDir backup{"orca_plate_overrides_load"};
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    ~Loaded()
    {
        release_PlateData_list(plates);
        for (Preset* preset : presets) delete preset;
    }

    void load(const std::string& path)
    {
        model.set_backup_path(backup.string());
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        bool bbs = false, orca = false;
        Semver version;
        REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitutions, &model, &plates, &presets, &bbs, &orca, &version,
                             nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        REQUIRE(plates.size() == 1);
    }
};

std::string read_model_settings(const std::string& path)
{
    struct Reader {
        mz_zip_archive zip{};
        ~Reader() { if (zip.m_pState) close_zip_reader(&zip); }
    } reader;
    REQUIRE(open_zip_reader(&reader.zip, path));
    const int index = mz_zip_reader_locate_file(&reader.zip, "Metadata/model_settings.config", nullptr, 0);
    REQUIRE(index >= 0);
    mz_zip_archive_file_stat stat;
    REQUIRE(mz_zip_reader_file_stat(&reader.zip, mz_uint(index), &stat));
    std::string data(size_t(stat.m_uncomp_size), '\0');
    REQUIRE(mz_zip_reader_extract_to_mem(&reader.zip, mz_uint(index), data.data(), data.size(), 0));
    return data;
}

} // namespace

TEST_CASE("Process settings on a plate survive a 3MF round trip", "[PlateOverrides]")
{
    ScopedTemporaryFile file{".3mf"};
    Project project;
    project.plate_config().set_key_value("layer_height", new ConfigOptionFloat(0.12));
    project.plate_config().set_key_value("wall_loops", new ConfigOptionInt(5));
    project.save(file.string());

    Loaded loaded;
    loaded.load(file.string());
    const DynamicPrintConfig& plate = loaded.plates.front()->config;

    REQUIRE(plate.has("layer_height"));
    CHECK_THAT(plate.opt_float("layer_height"), WithinAbs(0.12, 1e-9));
    REQUIRE(plate.has("wall_loops"));
    CHECK(plate.opt_int("wall_loops") == 5);
}

TEST_CASE("A plate without process settings is saved as upstream saves it", "[PlateOverrides]")
{
    ScopedTemporaryFile file{".3mf"};
    Project project;
    project.plate_config().set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    project.save(file.string());

    const std::string settings = read_model_settings(file.string());
    CHECK(settings.find(MultiPrinter::PLATE_OVERRIDE_PREFIX) == std::string::npos);
    CHECK(settings.find("print_sequence") != std::string::npos);
}

TEST_CASE("Only process settings that are not native plate keys count as plate overrides", "[PlateOverrides]")
{
    CHECK(MultiPrinter::is_plate_override_key("layer_height"));
    CHECK(MultiPrinter::is_plate_override_key("sparse_infill_density"));
    // Native plate keys keep their own storage in the plate block.
    CHECK_FALSE(MultiPrinter::is_plate_override_key("print_sequence"));
    CHECK_FALSE(MultiPrinter::is_plate_override_key("spiral_mode"));
    CHECK_FALSE(MultiPrinter::is_plate_override_key("curr_bed_type"));
    // Printer and filament settings are not process settings.
    CHECK_FALSE(MultiPrinter::is_plate_override_key("printable_area"));
    CHECK_FALSE(MultiPrinter::is_plate_override_key("nozzle_temperature"));
    CHECK_FALSE(MultiPrinter::is_plate_override_key("no_such_setting"));
}

TEST_CASE("Reading a plate override skips settings this build does not allow", "[PlateOverrides]")
{
    DynamicPrintConfig plate;
    // Not an override key: the caller handles it.
    CHECK_FALSE(MultiPrinter::read_plate_override(plate, "plater_name", "Plate"));
    // Override prefix, but unknown or not a process setting: consumed and ignored.
    CHECK(MultiPrinter::read_plate_override(plate, std::string(MultiPrinter::PLATE_OVERRIDE_PREFIX) + "no_such_setting", "1"));
    CHECK(MultiPrinter::read_plate_override(plate, std::string(MultiPrinter::PLATE_OVERRIDE_PREFIX) + "printable_area", "0x0"));
    CHECK(plate.keys().empty());
    // A valid process setting is stored.
    CHECK(MultiPrinter::read_plate_override(plate, std::string(MultiPrinter::PLATE_OVERRIDE_PREFIX) + "wall_loops", "4"));
    REQUIRE(plate.has("wall_loops"));
    CHECK(plate.opt_int("wall_loops") == 4);
}
