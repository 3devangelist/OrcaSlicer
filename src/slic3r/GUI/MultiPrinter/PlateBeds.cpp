// [multi-printer] Each plate shows its printer's bed model. See PlateBeds.hpp and fork/DESIGN.md.

#include "PlateBeds.hpp"

#include "PlatePresets.hpp"
#include "PlateSettings.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp" // make_counter_clockwise
#include "libslic3r/Geometry.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/3DBed.hpp"
#include "slic3r/GUI/3DScene.hpp" // glsafe
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <glad/gl.h>

#include <array>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace Slic3r {
namespace GUI {
namespace MultiPrinterGui {

namespace {

// Same as Bed3D (GROUND_Z there, and the -0.41 of update_model_offset).
constexpr double MODEL_Z = -0.41 - 0.04;

// The bed model of a printer, chosen as Bed3D::set_shape and detect_type choose it.
std::string find_model_file(const std::string& preset_name, const DynamicPrintConfig& config, const Pointfs& shape)
{
    std::string file;
    if (const auto* custom = config.option<ConfigOptionString>("bed_custom_model"); custom != nullptr && !custom->value.empty())
        file = custom->value;
    else if (PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr) {
        const Preset* curr = bundle->printers.find_preset(preset_name, false);
        while (curr != nullptr && file.empty()) {
            if (const auto* area = curr->config.option<ConfigOptionPoints>("printable_area");
                area != nullptr && shape == make_counter_clockwise(area->values)) {
                if (curr->is_system)
                    file = PresetUtils::system_printer_bed_model(*curr);
                else if (const auto* model = curr->config.option<ConfigOptionString>("printer_model");
                         model != nullptr && !model->value.empty())
                    file = bundle->get_stl_model_for_printer_model(model->value);
            }
            curr = bundle->printers.get_preset_parent(*curr);
        }
    }
    boost::system::error_code ec;
    if (!file.empty() && !(boost::algorithm::iends_with(file, ".stl") && boost::filesystem::exists(file, ec)))
        file.clear();
    return file;
}

// The model file of each printer: looked up once, it touches the disk.
const std::string& model_file(const std::string& preset_name, const DynamicPrintConfig& config, const Pointfs& shape)
{
    static std::map<std::string, std::string> files;
    std::ostringstream key;
    key << preset_name << '|';
    if (const auto* custom = config.option<ConfigOptionString>("bed_custom_model"))
        key << custom->value;
    for (const Vec2d& p : shape)
        key << '|' << p.x() << ',' << p.y();
    auto it = files.find(key.str());
    if (it == files.end())
        it = files.emplace(key.str(), find_model_file(preset_name, config, shape)).first;
    return it->second;
}

// Loaded models stay for the session: a few beds, each loaded once.
GLModel* loaded_model(const std::string& file)
{
    static std::map<std::string, std::unique_ptr<GLModel>> models;
    auto it = models.find(file);
    if (it == models.end()) {
        auto model = std::make_unique<GLModel>();
        if (!model->init_from_file(file)) {
            BOOST_LOG_TRIVIAL(error) << "[multi-printer] unable to load bed model " << file;
            model.reset();
        }
        it = models.emplace(file, std::move(model)).first;
    }
    return it->second.get();
}

// Owner's decision (2026-11-09): the beds of the plates not being looked at are light grey.
const ColorRGBA OTHER_COLOR      = {0.78f, 0.78f, 0.78f, 1.0f};
const ColorRGBA OTHER_COLOR_DARK = {0.45f, 0.45f, 0.47f, 1.0f};

void render_plate(PartPlate& plate, bool current, const Transform3d& view_matrix, const Transform3d& projection_matrix, GLShaderProgram& shader)
{
    std::string               name;
    const DynamicPrintConfig& config = plate_printer_config(&plate, name);
    const auto*               area   = config.option<ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.size() < 3)
        return;
    const Pointfs      shape = make_counter_clockwise(area->values); // as Plater::set_bed_shape passes it
    const std::string& file  = model_file(name, config, shape);
    if (file.empty())
        return; // no model: upstream draws the plain plate
    GLModel* model = loaded_model(file);
    if (model == nullptr)
        return;

    // Placed as Bed3D::update_model_offset places it.
    const Vec3d   origin = plate.get_origin();
    const Vec2d   offset(origin.x(), origin.y());
    BoundingBoxf  box = get_extents(shape);
    Vec3d         shift(box.center().x() + offset.x(), box.center().y() + offset.y(), MODEL_Z);
    if (file.find("bbl-3dp-") != std::string::npos) { // upstream's hack for the BBL models
        shift.x() -= shape[2].x() / 2.;
        shift.y() -= shape[2].y() / 2.;
    }

    // The printable volume of each extruder, for the shader to tint (as Bed3D::render_model).
    std::vector<Pointfs> extruder_areas;
    std::vector<double>  extruder_heights;
    if (const auto* o = config.option<ConfigOptionPointsGroups>("extruder_printable_area"))
        extruder_areas = o->values;
    if (const auto* o = config.option<ConfigOptionFloatsNullable>("extruder_printable_height"))
        extruder_heights = o->values;
    if (!extruder_areas.empty()) {
        Pointfs moved = shape;
        for (Vec2d& p : moved)
            p += offset;
        for (Pointfs& a : extruder_areas)
            for (Vec2d& p : a)
                p += offset;
        const auto*       height = config.option<ConfigOptionFloat>("printable_height");
        const BuildVolume volume(moved, height != nullptr ? height->value : 0., extruder_areas, extruder_heights);
        if (volume.get_extruder_area_count() > 0) {
            const BuildVolume::BuildSharedVolume& shared = volume.get_shared_volume();
            shader.set_uniform("print_volume.type", shared.type);
            shader.set_uniform("print_volume.xy_data", shared.data);
            std::array<float, 2> zs = shared.zs;
            zs[0]                   = -1;
            shader.set_uniform("print_volume.z_data", zs);
        } else
            shader.set_uniform("print_volume.type", -1);
    } else
        shader.set_uniform("print_volume.type", -1);

    const Transform3d model_matrix = Geometry::assemble_transform(shift);
    shader.set_uniform("volume_world_matrix", model_matrix);
    shader.set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader.set_uniform("projection_matrix", projection_matrix);
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                        model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader.set_uniform("view_normal_matrix", view_normal_matrix);
    model->set_color(current ? (wxGetApp().dark_mode() ? Bed3D::DEFAULT_MODEL_COLOR_DARK : Bed3D::DEFAULT_MODEL_COLOR) :
                               (wxGetApp().dark_mode() ? OTHER_COLOR_DARK : OTHER_COLOR));
    model->render();
}

} // namespace

bool plate_bed_models_enabled()
{
    return plate_process_settings_enabled() && wxGetApp().plater() != nullptr && wxGetApp().preset_bundle != nullptr;
}

void render_plate_bed_models(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current)
{
    if (bottom || !plate_bed_models_enabled())
        return;
    GLShaderProgram* shader = wxGetApp().get_shader("hotbed");
    if (shader == nullptr)
        return;
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    glsafe(::glEnable(GL_DEPTH_TEST));
    shader->start_using();
    shader->set_uniform("emission_factor", 0.0f);
    const PartPlate* current = plates.get_curr_plate();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (PartPlate* plate = plates.get_plate(i); plate != nullptr && (!only_current || plate == current))
            render_plate(*plate, plate == current, view_matrix, projection_matrix, *shader);
    shader->stop_using();
    glsafe(::glDisable(GL_DEPTH_TEST));
}

} // namespace MultiPrinterGui
} // namespace GUI
} // namespace Slic3r
