#include "mbs/infrastructure/sqlite/Repositories.hpp"

#include "mbs/domain/Validation.hpp"
#include "mbs/infrastructure/sqlite/Database.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mbs::infrastructure::sqlite {
namespace {

void ensure_ready(DatabaseManager& manager) { static_cast<void>(manager.ensure()); }

domain::DesignSource parse_design_source(const std::string_view value) {
    if (value == "manual") {
        return domain::DesignSource::manual;
    }
    if (value == "bo") {
        return domain::DesignSource::bayesian_optimization;
    }
    if (value == "existing" || value == "legacy") {
        return domain::DesignSource::existing;
    }
    return domain::DesignSource::manual;
}

std::string design_source_token(const domain::DesignSource source) {
    if (source == domain::DesignSource::bayesian_optimization) {
        return "bo";
    }
    return source == domain::DesignSource::existing ? "legacy" : "manual";
}

domain::TaskStatus parse_task_status(const std::string_view value) {
    if (value == "queued") {
        return domain::TaskStatus::queued;
    }
    if (value == "running") {
        return domain::TaskStatus::running;
    }
    if (value == "succeeded") {
        return domain::TaskStatus::succeeded;
    }
    if (value == "failed") {
        return domain::TaskStatus::failed;
    }
    if (value == "cancelled") {
        return domain::TaskStatus::cancelled;
    }
    if (value == "interrupted") {
        return domain::TaskStatus::interrupted;
    }
    throw SqliteError{SQLITE_CORRUPT, "unknown task status in database"};
}

domain::HyperelasticModel parse_hyperelastic_model(const std::string_view value) {
    if (value == "mooney_rivlin") {
        return domain::HyperelasticModel::mooney_rivlin;
    }
    if (value == "neo_hooke") {
        return domain::HyperelasticModel::neo_hooke;
    }
    if (value == "yeoh") {
        return domain::HyperelasticModel::yeoh;
    }
    if (value == "ogden") {
        return domain::HyperelasticModel::ogden;
    }
    throw SqliteError{SQLITE_CORRUPT, "unknown hyperelastic model in database"};
}

std::optional<std::string> existing_sample_id(Connection& connection,
                                              const std::optional<std::string>& requested,
                                              const std::string& project_id) {
    if (!requested.has_value() || requested->empty()) {
        return std::nullopt;
    }
    Statement query{connection, "SELECT sample_id FROM samples WHERE project_id=? AND sample_id=?"};
    query.bind(1, project_id);
    query.bind(2, *requested);
    return query.step() ? requested : std::nullopt;
}

constexpr std::string_view sample_upsert_sql = R"sql(
INSERT INTO samples(
 sample_id,project_id,dataset,serial,source,status,lmd,mu,kpa,bta,rnd_x,rnd_y,
 wth,rep_z,thk_p,resolution,len_pct,max_attempts,tet_generated,
 artifact_dir,tet_order,tet_mindihedral,tet_minratio,tet_nobisect,tet_quality,
 created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(sample_id) DO UPDATE SET
 project_id=excluded.project_id,dataset=excluded.dataset,serial=excluded.serial,
 status=excluded.status,lmd=excluded.lmd,mu=excluded.mu,kpa=excluded.kpa,bta=excluded.bta,
 rnd_x=excluded.rnd_x,rnd_y=excluded.rnd_y,wth=excluded.wth,rep_z=excluded.rep_z,
 thk_p=excluded.thk_p,resolution=excluded.resolution,len_pct=excluded.len_pct,
 max_attempts=excluded.max_attempts,tet_generated=excluded.tet_generated,
 artifact_dir=COALESCE(excluded.artifact_dir,samples.artifact_dir),
 tet_order=excluded.tet_order,tet_mindihedral=excluded.tet_mindihedral,
 tet_minratio=excluded.tet_minratio,tet_nobisect=excluded.tet_nobisect,
 tet_quality=excluded.tet_quality,updated_at=excluded.updated_at
)sql";

// The optimization table edits only parameters, result (through the observation
// repository), and status. Existing 3.0 metadata must therefore remain byte-for-byte
// untouched when the complete visible table is saved.
constexpr std::string_view sample_table_upsert_sql = R"sql(
INSERT INTO samples(
 sample_id,project_id,dataset,serial,source,status,lmd,mu,kpa,bta,rnd_x,rnd_y,
 wth,rep_z,thk_p,resolution,len_pct,max_attempts,tet_generated,
 artifact_dir,tet_order,tet_mindihedral,tet_minratio,tet_nobisect,tet_quality,
 created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(sample_id) DO UPDATE SET
 status=excluded.status,lmd=excluded.lmd,mu=excluded.mu,kpa=excluded.kpa,
 bta=excluded.bta,rnd_x=excluded.rnd_x,rnd_y=excluded.rnd_y,
 wth=excluded.wth,rep_z=excluded.rep_z,thk_p=excluded.thk_p,
 resolution=excluded.resolution,len_pct=excluded.len_pct,
 max_attempts=excluded.max_attempts,tet_generated=excluded.tet_generated,
 tet_order=excluded.tet_order,tet_mindihedral=excluded.tet_mindihedral,
 tet_minratio=excluded.tet_minratio,tet_nobisect=excluded.tet_nobisect,
 tet_quality=excluded.tet_quality,updated_at=excluded.updated_at
)sql";

void bind_sample(Statement& statement, const domain::Sample& sample, const std::string& project_id,
                 const std::string& timestamp) {
    statement.bind(1, sample.id);
    statement.bind(2, project_id);
    statement.bind(3, domain::to_string(sample.dataset));
    statement.bind(4, sample.serial);
    statement.bind(5, design_source_token(sample.source));
    statement.bind(6, sample.status);
    statement.bind(7, sample.parameters.lambda);
    statement.bind(8, sample.parameters.mu);
    statement.bind(9, sample.parameters.kappa);
    statement.bind(10, sample.parameters.beta);
    statement.bind(11, sample.parameters.phase_x);
    statement.bind(12, sample.parameters.phase_y);
    statement.bind(13, sample.mesh.width);
    statement.bind(14, sample.mesh.repeat_z);
    statement.bind(15, sample.mesh.plate_thickness);
    statement.bind(16, sample.mesh.resolution);
    statement.bind(17, sample.mesh.target_edge_percent);
    statement.bind(18, sample.mesh.max_attempts);
    statement.bind(19, sample.mesh.tetrahedralize ? 1 : 0);
    statement.bind_optional(20, sample.artifact_directory);
    statement.bind(21, sample.mesh.tetgen.order);
    statement.bind(22, sample.mesh.tetgen.minimum_dihedral);
    statement.bind(23, sample.mesh.tetgen.minimum_ratio);
    statement.bind(24, sample.mesh.tetgen.no_bisect ? 1 : 0);
    statement.bind(25, sample.mesh.tetgen.quality ? 1 : 0);
    statement.bind(26, timestamp);
    statement.bind(27, timestamp);
}

void validate_sample_scope(const domain::Sample& sample, const domain::DatasetKind dataset,
                           const std::string& project_id) {
    domain::require_valid(sample.validation_errors());
    if (sample.dataset != dataset || sample.project_id != project_id) {
        throw std::invalid_argument{"sample does not belong to repository dataset/project"};
    }
}

domain::Sample read_sample(Statement& row) {
    domain::Sample sample;
    sample.id = row.column_text(0);
    sample.project_id = row.column_text(1);
    sample.dataset =
        row.column_text(2) == "mbs" ? domain::DatasetKind::mbs : domain::DatasetKind::demo;
    sample.serial = row.column_int(3);
    sample.source = parse_design_source(row.column_text(4));
    sample.status = row.column_text(5);
    sample.parameters = {
        .lambda = row.column_double(6),
        .mu = row.column_double(7),
        .kappa = row.column_double(8),
        .beta = row.column_double(9),
        .phase_x = row.column_double(10),
        .phase_y = row.column_double(11),
    };
    sample.mesh.width = row.column_double(12);
    sample.mesh.repeat_z = row.column_int(13);
    sample.mesh.plate_thickness = row.column_double(14);
    sample.mesh.resolution = row.column_int(15);
    sample.mesh.target_edge_percent = row.column_double(16);
    sample.mesh.max_attempts = row.column_int(17);
    sample.mesh.tetrahedralize = row.column_int(18) != 0;
    sample.mesh.tetgen.order = row.column_int(19);
    sample.mesh.tetgen.minimum_dihedral = row.column_double(20);
    sample.mesh.tetgen.minimum_ratio = row.column_double(21);
    sample.mesh.tetgen.no_bisect = row.column_int(22) != 0;
    sample.mesh.tetgen.quality = row.column_int(23) != 0;
    sample.artifact_directory = row.column_optional_text(24);
    domain::require_valid(sample.validation_errors());
    return sample;
}

void load_mesh_options(Connection& connection, domain::Sample& sample) {
    Statement query{
        connection,
        "SELECT sizing_mode,surface_tolerance_percent,minimum_edge_percent,"
        "maximum_edge_percent,remesh_iterations,feature_angle_degrees,sharpen,simplify,"
        "simplify_keep_ratio,repair_rounds,tet_target_edge_length_mm,tet_optimization_level "
        "FROM mbs4_mesh_options WHERE sample_id=?"};
    query.bind(1, sample.id);
    if (!query.step()) {
        return;
    }
    sample.mesh.sizing_mode = query.column_text(0) == "curvature_adaptive"
                                  ? domain::SurfaceSizingMode::curvature_adaptive
                                  : domain::SurfaceSizingMode::uniform;
    sample.mesh.surface_tolerance_percent = query.column_double(1);
    sample.mesh.minimum_edge_percent = query.column_double(2);
    sample.mesh.maximum_edge_percent = query.column_double(3);
    sample.mesh.remesh_iterations = query.column_int(4);
    sample.mesh.feature_angle_degrees = query.column_double(5);
    sample.mesh.sharpen = query.column_int(6) != 0;
    sample.mesh.simplify = query.column_int(7) != 0;
    sample.mesh.simplify_keep_ratio = query.column_double(8);
    sample.mesh.repair_rounds = query.column_int(9);
    sample.mesh.tetgen.target_edge_length_mm = query.column_double(10);
    sample.mesh.tetgen.optimization_level = query.column_int(11);
    domain::require_valid(sample.validation_errors());
}

void save_mesh_options(Connection& connection, const domain::Sample& sample) {
    Statement save{
        connection,
        "INSERT INTO mbs4_mesh_options(sample_id,sizing_mode,surface_tolerance_percent,"
        "minimum_edge_percent,maximum_edge_percent,remesh_iterations,feature_angle_degrees,"
        "sharpen,simplify,simplify_keep_ratio,repair_rounds,tet_target_edge_length_mm,"
        "tet_optimization_level,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(sample_id) DO UPDATE SET sizing_mode=excluded.sizing_mode,"
        "surface_tolerance_percent=excluded.surface_tolerance_percent,"
        "minimum_edge_percent=excluded.minimum_edge_percent,"
        "maximum_edge_percent=excluded.maximum_edge_percent,"
        "remesh_iterations=excluded.remesh_iterations,"
        "feature_angle_degrees=excluded.feature_angle_degrees,sharpen=excluded.sharpen,"
        "simplify=excluded.simplify,simplify_keep_ratio=excluded.simplify_keep_ratio,"
        "repair_rounds=excluded.repair_rounds,"
        "tet_target_edge_length_mm=excluded.tet_target_edge_length_mm,"
        "tet_optimization_level=excluded.tet_optimization_level,updated_at=excluded.updated_at"};
    save.bind(1, sample.id);
    save.bind(2, domain::to_string(sample.mesh.sizing_mode));
    save.bind(3, sample.mesh.surface_tolerance_percent);
    save.bind(4, sample.mesh.minimum_edge_percent);
    save.bind(5, sample.mesh.maximum_edge_percent);
    save.bind(6, sample.mesh.remesh_iterations);
    save.bind(7, sample.mesh.feature_angle_degrees);
    save.bind(8, sample.mesh.sharpen ? 1 : 0);
    save.bind(9, sample.mesh.simplify ? 1 : 0);
    save.bind(10, sample.mesh.simplify_keep_ratio);
    save.bind(11, sample.mesh.repair_rounds);
    save.bind(12, sample.mesh.tetgen.target_edge_length_mm);
    save.bind(13, sample.mesh.tetgen.optimization_level);
    save.bind(14, now_timestamp());
    save.execute();
}

constexpr std::string_view sample_select_columns =
    "sample_id,project_id,dataset,COALESCE(serial,0),COALESCE(source,'manual'),"
    "COALESCE(status,''),COALESCE(lmd,0),COALESCE(mu,0),COALESCE(kpa,0),COALESCE(bta,0),"
    "COALESCE(rnd_x,0),COALESCE(rnd_y,0),COALESCE(wth,10),COALESCE(rep_z,3),"
    "COALESCE(thk_p,1),COALESCE(resolution,30),COALESCE(len_pct,5),"
    "COALESCE(max_attempts,20),COALESCE(tet_generated,0),COALESCE(tet_order,1),"
    "COALESCE(tet_mindihedral,20),COALESCE(tet_minratio,1.1),"
    "COALESCE(tet_nobisect,1),COALESCE(tet_quality,1),artifact_dir";

std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string result{"\""};
    for (const char character : value) {
        result += character == '\"' ? "\"\"" : std::string(1, character);
    }
    result += '\"';
    return result;
}

std::string json_escape(const std::string_view value) {
    std::string result;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += static_cast<char>(character); break;
        }
    }
    return result;
}

std::string material_payload(const domain::MaterialDefinition& material) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << "{\"name\":\"" << json_escape(material.name)
           << "\",\"density\":{\"enabled\":" << (material.density.enabled ? "true" : "false")
           << ",\"value\":" << material.density.value
           << "},\"elastic\":{\"enabled\":" << (material.elastic.enabled ? "true" : "false")
           << ",\"youngs_modulus\":" << material.elastic.youngs_modulus
           << ",\"poissons_ratio\":" << material.elastic.poissons_ratio
           << "},\"plastic\":{\"enabled\":" << (material.plastic.enabled ? "true" : "false")
           << ",\"table\":[";
    for (std::size_t index = 0; index < material.plastic.table.size(); ++index) {
        if (index != 0) output << ',';
        output << '[' << material.plastic.table[index].yield_stress << ','
               << material.plastic.table[index].plastic_strain << ']';
    }
    output << "]},\"hyperelastic\":{\"enabled\":"
           << (material.hyperelastic.enabled ? "true" : "false") << ",\"model\":\""
           << domain::to_string(material.hyperelastic.model) << "\",\"order\":"
           << material.hyperelastic.order << ",\"coefficients\":{";
    bool first = true;
    for (const auto& [name, value] : material.hyperelastic.coefficients) {
        if (!first) output << ',';
        first = false;
        output << '"' << json_escape(name) << "\":" << value;
    }
    output << "}}}";
    return output.str();
}

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (!error) {
        return;
    }
    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error{"cannot publish compatibility export: " + error.message()};
    }
}

void require_changed_once(const Connection& connection, const char* operation) {
    if (connection.changes() != 1) {
        throw SqliteError{SQLITE_NOTFOUND, std::string{operation} + " affected no row"};
    }
}

} // namespace

SqliteSampleRepository::SqliteSampleRepository(
    std::filesystem::path path, std::string project_id,
    std::filesystem::path compatibility_export_directory)
    : manager_(std::move(path), std::move(project_id)),
      compatibility_export_directory_(std::move(compatibility_export_directory)) {
    ensure_ready(manager_);
}

std::vector<domain::Sample> SqliteSampleRepository::list(const domain::DatasetKind dataset) const {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Statement query{connection,
                    "SELECT " + std::string{sample_select_columns} +
                        " FROM samples WHERE project_id=? AND dataset=? ORDER BY id"};
    query.bind(1, manager_.project_id());
    query.bind(2, domain::to_string(dataset));
    std::vector<domain::Sample> samples;
    while (query.step()) {
        auto sample = read_sample(query);
        load_mesh_options(connection, sample);
        samples.push_back(std::move(sample));
    }
    return samples;
}

void SqliteSampleRepository::replace(const domain::DatasetKind dataset,
                                     const std::span<const domain::Sample> samples) {
    ensure_ready(manager_);
    for (const auto& sample : samples) {
        validate_sample_scope(sample, dataset, manager_.project_id());
    }
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    connection.execute("CREATE TEMP TABLE retained_samples(sample_id TEXT PRIMARY KEY)");
    Statement upsert{connection, sample_table_upsert_sql};
    Statement retain{connection, "INSERT INTO retained_samples(sample_id) VALUES(?)"};
    for (const auto& sample : samples) {
        bind_sample(upsert, sample, manager_.project_id(), now_timestamp());
        upsert.execute();
        upsert.reset();
        save_mesh_options(connection, sample);
        retain.bind(1, sample.id);
        retain.execute();
        retain.reset();
    }
    Statement remove{connection,
                     "DELETE FROM samples WHERE project_id=? AND dataset=? AND sample_id NOT IN "
                     "(SELECT sample_id FROM retained_samples)"};
    remove.bind(1, manager_.project_id());
    remove.bind(2, domain::to_string(dataset));
    remove.execute();
    connection.execute("DROP TABLE retained_samples");
    transaction.commit();
    export_dataset(dataset);
}

void SqliteSampleRepository::save(const domain::Sample& sample) {
    validate_sample_scope(sample, sample.dataset, manager_.project_id());
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement upsert{connection, sample_upsert_sql};
    bind_sample(upsert, sample, manager_.project_id(), now_timestamp());
    upsert.execute();
    save_mesh_options(connection, sample);
    transaction.commit();
    export_dataset(sample.dataset);
}

void SqliteSampleRepository::export_dataset(const domain::DatasetKind dataset) const {
    if (compatibility_export_directory_.empty()) {
        return;
    }
    std::filesystem::create_directories(compatibility_export_directory_);
    const auto destination = compatibility_export_directory_ /
                             (dataset == domain::DatasetKind::mbs ? "mbs_guess.csv" : "demo.csv");
    const auto temporary = destination.string() + ".tmp";
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"cannot create compatibility CSV"};
    }
    static constexpr std::string_view header =
        "lmd,mu,kpa,bta,result,%,rnd_x,rnd_y,sample_id,serial,source,status,wth,rep_z,"
        "thk_p,m,len_pct,max_attempts,tet_generated,artifact_dir,tet_order,tet_mindihedral,"
        "tet_minratio,tet_nobisect,tet_quality,last_simulation_dir,created_at,updated_at,last_error";
    output << header << '\n';
    Connection connection{manager_.path()};
    Statement rows{connection,
                   "SELECT COALESCE(CAST(lmd AS TEXT),''),COALESCE(CAST(mu AS TEXT),''),"
                   "COALESCE(CAST(kpa AS TEXT),''),COALESCE(CAST(bta AS TEXT),''),"
                   "COALESCE(CAST(result AS TEXT),''),COALESCE(CAST(result_percent AS TEXT),''),"
                   "COALESCE(CAST(rnd_x AS TEXT),''),COALESCE(CAST(rnd_y AS TEXT),''),"
                   "COALESCE(sample_id,''),COALESCE(CAST(serial AS TEXT),''),COALESCE(source,''),"
                   "COALESCE(status,''),COALESCE(CAST(wth AS TEXT),''),"
                   "COALESCE(CAST(rep_z AS TEXT),''),COALESCE(CAST(thk_p AS TEXT),''),"
                   "COALESCE(CAST(resolution AS TEXT),''),COALESCE(CAST(len_pct AS TEXT),''),"
                   "COALESCE(CAST(max_attempts AS TEXT),''),COALESCE(CAST(tet_generated AS TEXT),''),"
                   "COALESCE(artifact_dir,''),COALESCE(CAST(tet_order AS TEXT),''),"
                   "COALESCE(CAST(tet_mindihedral AS TEXT),''),COALESCE(CAST(tet_minratio AS TEXT),''),"
                   "COALESCE(CAST(tet_nobisect AS TEXT),''),COALESCE(CAST(tet_quality AS TEXT),''),"
                   "COALESCE(last_simulation_dir,''),COALESCE(created_at,''),COALESCE(updated_at,''),"
                   "COALESCE(last_error,'') FROM samples WHERE project_id=? AND dataset=? ORDER BY id"};
    rows.bind(1, manager_.project_id());
    rows.bind(2, domain::to_string(dataset));
    while (rows.step()) {
        for (int column = 0; column < 29; ++column) {
            if (column != 0) {
                output << ',';
            }
            output << csv_escape(rows.column_text(column));
        }
        output << '\n';
    }
    output.close();
    replace_file_atomically(temporary, destination);
}

SqliteMaterialRepository::SqliteMaterialRepository(
    std::filesystem::path path, std::string project_id,
    std::filesystem::path compatibility_export_file)
    : manager_(std::move(path), std::move(project_id)),
      compatibility_export_file_(std::move(compatibility_export_file)) {
    ensure_ready(manager_);
}

domain::MaterialLibrary SqliteMaterialRepository::load() const {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Statement materials{
        connection,
        "SELECT payload_json,name,COALESCE(json_extract(payload_json,'$.density.enabled'),0),"
        "COALESCE(json_extract(payload_json,'$.density.value'),0),"
        "COALESCE(json_extract(payload_json,'$.elastic.enabled'),1),"
        "COALESCE(json_extract(payload_json,'$.elastic.youngs_modulus'),0),"
        "COALESCE(json_extract(payload_json,'$.elastic.poissons_ratio'),0),"
        "COALESCE(json_extract(payload_json,'$.plastic.enabled'),0),"
        "COALESCE(json_extract(payload_json,'$.hyperelastic.enabled'),0),"
        "COALESCE(json_extract(payload_json,'$.hyperelastic.model'),'mooney_rivlin'),"
        "COALESCE(json_extract(payload_json,'$.hyperelastic.order'),1) "
        "FROM materials WHERE project_id=? ORDER BY rowid"};
    materials.bind(1, manager_.project_id());
    domain::MaterialLibrary library;
    while (materials.step()) {
        const auto payload = materials.column_text(0);
        domain::MaterialDefinition material;
        material.name = materials.column_text(1);
        material.density = {.enabled = materials.column_int(2) != 0,
                            .value = materials.column_double(3)};
        material.elastic = {.enabled = materials.column_int(4) != 0,
                            .youngs_modulus = materials.column_double(5),
                            .poissons_ratio = materials.column_double(6)};
        material.plastic.enabled = materials.column_int(7) != 0;
        material.hyperelastic.enabled = materials.column_int(8) != 0;
        material.hyperelastic.model = parse_hyperelastic_model(materials.column_text(9));
        material.hyperelastic.order = materials.column_int(10);

        Statement points{connection,
                         "SELECT json_extract(value,'$[0]'),json_extract(value,'$[1]') "
                         "FROM json_each(json_extract(?,'$.plastic.table')) ORDER BY key"};
        points.bind(1, payload);
        while (points.step()) {
            material.plastic.table.push_back({.yield_stress = points.column_double(0),
                                              .plastic_strain = points.column_double(1)});
        }
        Statement coefficients{connection,
                               "SELECT key,value FROM json_each(json_extract(?,"
                               "'$.hyperelastic.coefficients')) ORDER BY key"};
        coefficients.bind(1, payload);
        while (coefficients.step()) {
            material.hyperelastic.coefficients.emplace(coefficients.column_text(0),
                                                       coefficients.column_double(1));
        }
        library.materials.push_back(std::move(material));
    }
    domain::require_valid(library.validation_errors());
    return library;
}

void SqliteMaterialRepository::replace(const domain::MaterialLibrary& library) {
    domain::require_valid(library.validation_errors());
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement remove{connection, "DELETE FROM materials WHERE project_id=?"};
    remove.bind(1, manager_.project_id());
    remove.execute();

    Statement material_insert{
        connection,
        "INSERT INTO materials(material_id,project_id,name,payload_json,updated_at) "
        "VALUES(?,?,?,?,?)"};
    for (const auto& material : library.materials) {
        const auto material_id = generate_id("material");
        material_insert.bind(1, material_id);
        material_insert.bind(2, manager_.project_id());
        material_insert.bind(3, material.name);
        material_insert.bind(4, material_payload(material));
        material_insert.bind(5, now_timestamp());
        material_insert.execute();
        material_insert.reset();
    }
    transaction.commit();
    export_library(library);
}

void SqliteMaterialRepository::export_library(const domain::MaterialLibrary& library) const {
    if (compatibility_export_file_.empty()) {
        return;
    }
    std::filesystem::create_directories(compatibility_export_file_.parent_path());
    const auto temporary = compatibility_export_file_.string() + ".tmp";
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"cannot create materials compatibility export"};
    }
    output << "{\n  \"schema_version\": 1,\n  \"materials\": [";
    for (std::size_t index = 0; index < library.materials.size(); ++index) {
        output << (index == 0 ? "\n    " : ",\n    ") << material_payload(library.materials[index]);
    }
    output << "\n  ]\n}\n";
    output.close();
    replace_file_atomically(temporary, compatibility_export_file_);
}

SqliteOptimizationStateRepository::SqliteOptimizationStateRepository(
    std::filesystem::path path, std::string project_id,
    std::filesystem::path compatibility_export_directory)
    : manager_(std::move(path), std::move(project_id)),
      compatibility_export_directory_(std::move(compatibility_export_directory)) {
    ensure_ready(manager_);
}

std::optional<std::string> SqliteOptimizationStateRepository::load_pending_json() const {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Statement query{connection, "SELECT payload_json FROM pending_optimization WHERE project_id=?"};
    query.bind(1, manager_.project_id());
    return query.step() ? std::optional<std::string>{query.column_text(0)} : std::nullopt;
}

void SqliteOptimizationStateRepository::save_pending_json(const std::string_view payload) {
    if (payload.empty()) {
        throw std::invalid_argument{"pending optimization payload must not be empty"};
    }
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement save{connection,
                   "INSERT INTO pending_optimization(project_id,payload_json,updated_at) "
                   "VALUES(?,?,?) ON CONFLICT(project_id) DO UPDATE SET "
                   "payload_json=excluded.payload_json,updated_at=excluded.updated_at"};
    save.bind(1, manager_.project_id());
    save.bind(2, payload);
    save.bind(3, now_timestamp());
    save.execute();
    transaction.commit();
}

void SqliteOptimizationStateRepository::clear_pending() {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement clear{connection, "DELETE FROM pending_optimization WHERE project_id=?"};
    clear.bind(1, manager_.project_id());
    clear.execute();
    transaction.commit();
}

std::vector<domain::OptimizationObservation>
SqliteOptimizationStateRepository::observations() const {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Statement query{connection,
                    "SELECT o.observation_id,o.sample_id,o.objective,o.lmd,o.mu,o.kpa,o.bta,"
                    "COALESCE(s.rnd_x,0),COALESCE(s.rnd_y,0) FROM optimization_observations o "
                    "LEFT JOIN samples s ON s.sample_id=o.sample_id "
                    "WHERE o.project_id=? ORDER BY o.rowid"};
    query.bind(1, manager_.project_id());
    std::vector<domain::OptimizationObservation> result;
    while (query.step()) {
        result.push_back({
            .id = query.column_text(0),
            .sample_id = query.column_text(1),
            .objective = query.column_double(2),
            .parameters = {.lambda = query.column_double(3),
                           .mu = query.column_double(4),
                           .kappa = query.column_double(5),
                           .beta = query.column_double(6),
                           .phase_x = query.column_double(7),
                           .phase_y = query.column_double(8)},
        });
    }
    return result;
}

void SqliteOptimizationStateRepository::save_observation(
    const domain::OptimizationObservation& observation) {
    domain::require_valid(observation.parameters.validation_errors());
    if (observation.id.empty() || observation.sample_id.empty() ||
        !std::isfinite(observation.objective)) {
        throw std::invalid_argument{"optimization observation is incomplete"};
    }
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement save{connection,
                   "INSERT INTO optimization_observations(observation_id,project_id,sample_id,"
                   "lmd,mu,kpa,bta,objective,created_at) "
                   "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(sample_id) DO UPDATE SET "
                   "observation_id=excluded.observation_id,lmd=excluded.lmd,mu=excluded.mu,"
                   "kpa=excluded.kpa,bta=excluded.bta,objective=excluded.objective"};
    save.bind(1, observation.id);
    save.bind(2, manager_.project_id());
    save.bind(3, observation.sample_id);
    save.bind(4, observation.parameters.lambda);
    save.bind(5, observation.parameters.mu);
    save.bind(6, observation.parameters.kappa);
    save.bind(7, observation.parameters.beta);
    save.bind(8, observation.objective);
    save.bind(9, now_timestamp());
    save.execute();
    Statement result{connection, "UPDATE samples SET result=?,updated_at=? "
                                 "WHERE project_id=? AND sample_id=?"};
    result.bind(1, observation.objective);
    result.bind(2, now_timestamp());
    result.bind(3, manager_.project_id());
    result.bind(4, observation.sample_id);
    result.execute();
    transaction.commit();
    refresh_mbs_export();
}

void SqliteOptimizationStateRepository::delete_observation(const std::string_view sample_id) {
    if (sample_id.empty()) {
        throw std::invalid_argument{"optimization sample id must not be empty"};
    }
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement remove{
        connection,
        "DELETE FROM optimization_observations WHERE project_id=? AND sample_id=?"};
    remove.bind(1, manager_.project_id());
    remove.bind(2, sample_id);
    remove.execute();
    Statement clear_result{connection, "UPDATE samples SET result=NULL,updated_at=? "
                                       "WHERE project_id=? AND sample_id=?"};
    clear_result.bind(1, now_timestamp());
    clear_result.bind(2, manager_.project_id());
    clear_result.bind(3, sample_id);
    clear_result.execute();
    transaction.commit();
    refresh_mbs_export();
}

void SqliteOptimizationStateRepository::refresh_mbs_export() const {
    if (compatibility_export_directory_.empty()) {
        return;
    }
    SqliteSampleRepository samples{manager_.path(), manager_.project_id(),
                                   compatibility_export_directory_};
    samples.export_dataset(domain::DatasetKind::mbs);
}

SqliteTaskRepository::SqliteTaskRepository(std::filesystem::path path, std::string project_id)
    : manager_(std::move(path), std::move(project_id)) {
    ensure_ready(manager_);
}

void SqliteTaskRepository::create(const domain::Task& task) {
    domain::require_valid(task.validation_errors());
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto sample_id = existing_sample_id(connection, task.sample_id, manager_.project_id());
    const auto timestamp = now_timestamp();
    Statement insert{connection,
                     "INSERT INTO tasks(task_id,project_id,run_id,sample_id,kind,status,progress,"
                     "error,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)"};
    insert.bind(1, task.id);
    insert.bind(2, manager_.project_id());
    insert.bind_optional(3, task.run_id);
    insert.bind_optional(4, sample_id);
    insert.bind(5, task.kind);
    insert.bind(6, domain::to_string(task.status));
    insert.bind(7, task.progress);
    insert.bind(8, task.error);
    insert.bind(9, timestamp);
    insert.bind(10, timestamp);
    insert.execute();
    transaction.commit();
}

std::optional<domain::Task> SqliteTaskRepository::find(const std::string_view task_id) const {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Statement query{connection,
                    "SELECT task_id,kind,status,sample_id,run_id,progress,error FROM tasks "
                    "WHERE project_id=? AND task_id=?"};
    query.bind(1, manager_.project_id());
    query.bind(2, task_id);
    if (!query.step()) {
        return std::nullopt;
    }
    return domain::Task{
        .id = query.column_text(0),
        .kind = query.column_text(1),
        .status = parse_task_status(query.column_text(2)),
        .sample_id = query.column_optional_text(3),
        .run_id = query.column_optional_text(4),
        .progress = query.column_double(5),
        .error = query.column_text(6),
    };
}

void SqliteTaskRepository::update(const domain::Task& task) {
    domain::require_valid(task.validation_errors());
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto timestamp = now_timestamp();
    Statement update{
        connection,
        "UPDATE tasks SET status=?,progress=?,error=?,sample_id=?,run_id=?,"
        "started_at=CASE WHEN ?='running' THEN COALESCE(started_at,?) ELSE started_at END,"
        "finished_at=CASE WHEN ? IN ('succeeded','failed','cancelled','interrupted') THEN ? "
        "ELSE finished_at END,updated_at=? WHERE project_id=? AND task_id=?"};
    const auto status = domain::to_string(task.status);
    update.bind(1, status);
    update.bind(2, task.progress);
    update.bind(3, task.error);
    update.bind_optional(4, task.sample_id);
    update.bind_optional(5, task.run_id);
    update.bind(6, status);
    update.bind(7, timestamp);
    update.bind(8, status);
    update.bind(9, timestamp);
    update.bind(10, timestamp);
    update.bind(11, manager_.project_id());
    update.bind(12, task.id);
    update.execute();
    require_changed_once(connection, "update task");
    transaction.commit();
}

int SqliteTaskRepository::interrupt_running() {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto timestamp = now_timestamp();
    Statement update{connection,
                     "UPDATE tasks SET status='interrupted',"
                     "error='Application restarted while task was running',finished_at=?,"
                     "updated_at=? WHERE project_id=? AND status IN ('queued','running')"};
    update.bind(1, timestamp);
    update.bind(2, timestamp);
    update.bind(3, manager_.project_id());
    update.execute();
    const int count = connection.changes();
    transaction.commit();
    return count;
}

SqliteTaskLifecycleStore::SqliteTaskLifecycleStore(std::filesystem::path path,
                                                   std::string project_id)
    : manager_(std::move(path), std::move(project_id)) {
    ensure_ready(manager_);
}

void SqliteTaskLifecycleStore::start(const domain::Task& task, const domain::Run& run) {
    domain::require_valid(task.validation_errors());
    if (task.run_id != run.id || task.sample_id != run.sample_id || task.kind != run.kind ||
        task.id.empty() || run.id.empty() || task.status != domain::TaskStatus::running ||
        run.status != domain::TaskStatus::running) {
        throw std::invalid_argument{"task and run identifiers are inconsistent"};
    }
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto sample_id = existing_sample_id(connection, run.sample_id, manager_.project_id());
    const auto timestamp = now_timestamp();
    Statement insert_run{
        connection,
        "INSERT INTO runs(run_id,project_id,sample_id,kind,status,request_json,error,created_at,"
        "started_at) VALUES(?,?,?,?,?,?,?,?,?)"};
    insert_run.bind(1, run.id);
    insert_run.bind(2, manager_.project_id());
    insert_run.bind_optional(3, sample_id);
    insert_run.bind(4, run.kind);
    insert_run.bind(5, domain::to_string(run.status));
    insert_run.bind(6, run.request_json);
    insert_run.bind(7, run.error);
    insert_run.bind(8, timestamp);
    insert_run.bind(9, timestamp);
    insert_run.execute();
    Statement insert_task{
        connection,
        "INSERT INTO tasks(task_id,project_id,run_id,sample_id,kind,status,progress,error,"
        "created_at,started_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)"};
    insert_task.bind(1, task.id);
    insert_task.bind(2, manager_.project_id());
    insert_task.bind(3, run.id);
    insert_task.bind_optional(4, sample_id);
    insert_task.bind(5, task.kind);
    insert_task.bind(6, domain::to_string(task.status));
    insert_task.bind(7, task.progress);
    insert_task.bind(8, task.error);
    insert_task.bind(9, timestamp);
    insert_task.bind(10, timestamp);
    insert_task.bind(11, timestamp);
    insert_task.execute();
    transaction.commit();
}

void SqliteTaskLifecycleStore::record_event(
    const std::string_view task_id, const std::string_view run_id,
    const std::optional<std::string>& requested_sample_id,
    const std::span<const application::ArtifactDraft> artifacts,
    const std::span<const application::MetricDraft> metrics, const std::optional<double> progress) {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    Statement execution_check{connection,
                              "SELECT 1 FROM tasks JOIN runs ON runs.run_id=tasks.run_id "
                              "WHERE tasks.project_id=? AND tasks.task_id=? AND tasks.run_id=?"};
    execution_check.bind(1, manager_.project_id());
    execution_check.bind(2, task_id);
    execution_check.bind(3, run_id);
    if (!execution_check.step()) {
        throw SqliteError{SQLITE_NOTFOUND, "task execution does not exist"};
    }
    const auto sample_id =
        existing_sample_id(connection, requested_sample_id, manager_.project_id());
    if (sample_id.has_value()) {
        Statement link_run{connection, "UPDATE runs SET sample_id=? WHERE run_id=?"};
        link_run.bind(1, *sample_id);
        link_run.bind(2, run_id);
        link_run.execute();
        require_changed_once(connection, "link run sample");
        Statement link_task{connection, "UPDATE tasks SET sample_id=? WHERE task_id=?"};
        link_task.bind(1, *sample_id);
        link_task.bind(2, task_id);
        link_task.execute();
        require_changed_once(connection, "link task sample");
    }
    Statement artifact_insert{
        connection,
        "INSERT INTO artifacts(artifact_id,project_id,sample_id,run_id,kind,uri,created_at) "
        "VALUES(?,?,?,?,?,?,?) ON CONFLICT(run_id,kind,uri) DO UPDATE SET "
        "sample_id=excluded.sample_id"};
    for (const auto& artifact : artifacts) {
        artifact_insert.bind(1, generate_id("artifact"));
        artifact_insert.bind(2, manager_.project_id());
        artifact_insert.bind_optional(3, sample_id);
        artifact_insert.bind(4, run_id);
        artifact_insert.bind(5, artifact.kind);
        artifact_insert.bind(6, artifact.uri);
        artifact_insert.bind(7, now_timestamp());
        artifact_insert.execute();
        artifact_insert.reset();
    }
    Statement metric_insert{
        connection, "INSERT INTO metrics(metric_id,project_id,sample_id,run_id,name,value,unit,"
                    "details_json,created_at) VALUES(?,?,?,?,?,?,?,?,?)"};
    for (const auto& metric : metrics) {
        metric_insert.bind(1, generate_id("metric"));
        metric_insert.bind(2, manager_.project_id());
        metric_insert.bind_optional(3, sample_id);
        metric_insert.bind(4, run_id);
        metric_insert.bind(5, metric.name);
        metric_insert.bind(6, metric.value);
        metric_insert.bind(7, metric.unit);
        metric_insert.bind(8, metric.details_json.empty() ? "{}" : metric.details_json);
        metric_insert.bind(9, now_timestamp());
        metric_insert.execute();
        metric_insert.reset();
    }
    if (progress.has_value()) {
        if (!std::isfinite(*progress) || *progress < 0.0 || *progress > 1.0) {
            throw std::invalid_argument{"task progress must be in [0, 1]"};
        }
        Statement update{connection, "UPDATE tasks SET progress=?,updated_at=? WHERE task_id=?"};
        update.bind(1, *progress);
        update.bind(2, now_timestamp());
        update.bind(3, task_id);
        update.execute();
        require_changed_once(connection, "record task progress");
    }
    transaction.commit();
}

void SqliteTaskLifecycleStore::finish(const std::string_view task_id, const std::string_view run_id,
                                      const domain::TaskStatus status,
                                      const std::string_view error) {
    if (!domain::is_terminal(status)) {
        throw std::invalid_argument{"finish requires a terminal status"};
    }
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto timestamp = now_timestamp();
    const auto status_text = domain::to_string(status);
    Statement task_update{
        connection,
        "UPDATE tasks SET status=?,progress=CASE WHEN ?='succeeded' THEN 1 ELSE progress END,"
        "error=?,finished_at=?,updated_at=? WHERE project_id=? AND task_id=? AND run_id=?"};
    task_update.bind(1, status_text);
    task_update.bind(2, status_text);
    task_update.bind(3, error);
    task_update.bind(4, timestamp);
    task_update.bind(5, timestamp);
    task_update.bind(6, manager_.project_id());
    task_update.bind(7, task_id);
    task_update.bind(8, run_id);
    task_update.execute();
    require_changed_once(connection, "finish task");
    Statement run_update{connection,
                         "UPDATE runs SET status=?,error=?,finished_at=? WHERE project_id=? "
                         "AND run_id=?"};
    run_update.bind(1, status_text);
    run_update.bind(2, error);
    run_update.bind(3, timestamp);
    run_update.bind(4, manager_.project_id());
    run_update.bind(5, run_id);
    run_update.execute();
    require_changed_once(connection, "finish run");
    transaction.commit();
}

int SqliteTaskLifecycleStore::recover_interrupted() {
    ensure_ready(manager_);
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto timestamp = now_timestamp();
    Statement tasks{connection,
                    "UPDATE tasks SET status='interrupted',"
                    "error='Application restarted while task was running',finished_at=?,"
                    "updated_at=? WHERE project_id=? AND status IN ('queued','running')"};
    tasks.bind(1, timestamp);
    tasks.bind(2, timestamp);
    tasks.bind(3, manager_.project_id());
    tasks.execute();
    const int count = connection.changes();
    Statement runs{connection, "UPDATE runs SET status='interrupted',"
                               "error='Application restarted while task was running',finished_at=? "
                               "WHERE project_id=? AND status IN ('queued','running')"};
    runs.bind(1, timestamp);
    runs.bind(2, manager_.project_id());
    runs.execute();
    transaction.commit();
    return count;
}

SqliteArtifactRepository::SqliteArtifactRepository(std::filesystem::path path,
                                                   std::string project_id)
    : manager_(std::move(path), std::move(project_id)) {
    ensure_ready(manager_);
}

std::string SqliteArtifactRepository::register_artifact(const domain::ArtifactRef& artifact) {
    if (artifact.kind.empty() || artifact.uri.empty()) {
        throw std::invalid_argument{"artifact kind and URI are required"};
    }
    ensure_ready(manager_);
    const auto artifact_id = artifact.id.empty() ? generate_id("artifact") : artifact.id;
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto sample_id =
        existing_sample_id(connection, artifact.sample_id, manager_.project_id());
    Statement insert{
        connection,
        "INSERT INTO artifacts(artifact_id,project_id,sample_id,run_id,kind,uri,size_bytes,"
        "checksum,created_at) VALUES(?,?,?,?,?,?,?,?,?)"};
    insert.bind(1, artifact_id);
    insert.bind(2, manager_.project_id());
    insert.bind_optional(3, sample_id);
    insert.bind_optional(4, artifact.run_id);
    insert.bind(5, artifact.kind);
    insert.bind(6, artifact.uri);
    insert.bind_optional(7, artifact.size_bytes);
    insert.bind_optional(8, artifact.checksum);
    insert.bind(9, now_timestamp());
    insert.execute();
    transaction.commit();
    return artifact_id;
}

SqliteMetricRepository::SqliteMetricRepository(std::filesystem::path path, std::string project_id)
    : manager_(std::move(path), std::move(project_id)) {
    ensure_ready(manager_);
}

std::string SqliteMetricRepository::record_metric(const domain::Metric& metric) {
    if (metric.name.empty() || !std::isfinite(metric.value)) {
        throw std::invalid_argument{"metric name and finite value are required"};
    }
    ensure_ready(manager_);
    const auto metric_id = metric.id.empty() ? generate_id("metric") : metric.id;
    Connection connection{manager_.path()};
    Transaction transaction{connection};
    const auto sample_id = existing_sample_id(connection, metric.sample_id, manager_.project_id());
    Statement insert{
        connection,
        "INSERT INTO metrics(metric_id,project_id,sample_id,run_id,name,value,unit,details_json,"
        "created_at) VALUES(?,?,?,?,?,?,?,?,?)"};
    insert.bind(1, metric_id);
    insert.bind(2, manager_.project_id());
    insert.bind_optional(3, sample_id);
    insert.bind_optional(4, metric.run_id);
    insert.bind(5, metric.name);
    insert.bind(6, metric.value);
    insert.bind(7, metric.unit);
    insert.bind(8, metric.details_json.empty() ? "{}" : metric.details_json);
    insert.bind(9, now_timestamp());
    insert.execute();
    transaction.commit();
    return metric_id;
}

} // namespace mbs::infrastructure::sqlite
