#include "mbs/infrastructure/abaqus/AbaqusGateway.hpp"
#include "mbs/infrastructure/sqlite/Repositories.hpp"
#include "mbs/geometry/ContactRisk.hpp"
#include "mbs/geometry/GeometryKernel.hpp"
#include "mbs/optimization/GaussianProcessOptimizer.hpp"
#include "mbs/domain/Validation.hpp"
#include "mbs/runtime/EventEnvelope.hpp"
#include "mbs/runtime/ProcessRunner.hpp"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using Options = std::map<std::string, std::string, std::less<>>;
mbs::runtime::CancellationToken* active_cancellation = nullptr;

#ifdef _WIN32
BOOL WINAPI console_handler(const DWORD signal) {
    if ((signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) &&
        active_cancellation != nullptr) {
        active_cancellation->request();
        return TRUE;
    }
    return FALSE;
}
#endif

Options parse_options(const int argc, char** argv, const int first) {
    Options options;
    for (int index = first; index < argc; ++index) {
        const std::string key{argv[index]};
        if (!key.starts_with("--") || index + 1 >= argc) {
            throw std::invalid_argument{"worker options must use --name value pairs"};
        }
        options[key.substr(2)] = argv[++index];
    }
    return options;
}

const std::string& required(const Options& options, const std::string_view key) {
    const auto found = options.find(key);
    if (found == options.end() || found->second.empty()) {
        throw std::invalid_argument{"missing required worker option --" + std::string{key}};
    }
    return found->second;
}

std::string value_or(const Options& options, const std::string_view key, std::string fallback) {
    const auto found = options.find(key);
    return found == options.end() ? std::move(fallback) : found->second;
}

double positive_double(const Options& options, const std::string_view key, const double fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    double value{};
    const auto result =
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    if (result.ec != std::errc{} || result.ptr != found->second.data() + found->second.size() ||
        value <= 0.0) {
        throw std::invalid_argument{"worker option --" + std::string{key} + " must be positive"};
    }
    return value;
}

double finite_double(const Options& options, const std::string_view key, const double fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    double value{};
    const auto result =
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    if (result.ec != std::errc{} || result.ptr != found->second.data() + found->second.size() ||
        !std::isfinite(value)) {
        throw std::invalid_argument{"worker option --" + std::string{key} + " must be finite"};
    }
    return value;
}

int integer_value(const Options& options, const std::string_view key, const int fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    int value{};
    const auto result =
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    if (result.ec != std::errc{} || result.ptr != found->second.data() + found->second.size()) {
        throw std::invalid_argument{"worker option --" + std::string{key} + " must be an integer"};
    }
    return value;
}

bool boolean_value(const Options& options, const std::string_view key, const bool fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    if (found->second == "1" || found->second == "true") {
        return true;
    }
    if (found->second == "0" || found->second == "false") {
        return false;
    }
    throw std::invalid_argument{"worker option --" + std::string{key} + " must be true/false"};
}

mbs::infrastructure::abaqus::AbaqusOperation operation_from(const std::string_view value) {
    using enum mbs::infrastructure::abaqus::AbaqusOperation;
    if (value == "simulation") {
        return simulation;
    }
    if (value == "preprocess-validation") {
        return preprocess_validation;
    }
    if (value == "postprocess") {
        return postprocess;
    }
    if (value == "animation") {
        return animation;
    }
    throw std::invalid_argument{"unsupported Abaqus worker operation: " + std::string{value}};
}

mbs::domain::AcquisitionFunction acquisition_from(const std::string_view value) {
    using enum mbs::domain::AcquisitionFunction;
    if (value == "EI") {
        return expected_improvement;
    }
    if (value == "LCB") {
        return lower_confidence_bound;
    }
    if (value == "PI") {
        return probability_improvement;
    }
    throw std::invalid_argument{"unsupported acquisition function: " + std::string{value}};
}

void emit(const mbs::runtime::EventEnvelope& event) {
    std::cout << event.encode() << '\n' << std::flush;
}

void health_check() {
    emit({
        .event = "completed",
        .task_id = "task-1",
        .run_id = {},
        .task_kind = "health-check",
        .sample_id = {},
        .message = "C++ worker ready",
        .progress = 1.0,
        .artifact_uris = {},
        .proof_stress = std::nullopt,
        .result_json = {},
    });
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"cannot read Abaqus result: " + path.string()};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<double> json_number(const std::string_view json, const std::string_view key) {
    const auto marker = '"' + std::string{key} + '"';
    auto position = json.find(marker);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    double value{};
    const auto result = std::from_chars(json.data() + position, json.data() + json.size(), value);
    if (result.ec != std::errc{} || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

int run_abaqus(const Options& options) {
    const auto task_id = required(options, "task-id");
    const auto operation_text = required(options, "operation");
    const auto operation = operation_from(operation_text);
    mbs::runtime::CancellationToken cancellation;
    mbs::runtime::SystemProcessRunner processes;
    mbs::infrastructure::abaqus::AbaqusGateway gateway{processes};
    const auto working_directory = std::filesystem::path{required(options, "work-dir")};
    const auto job_name = value_or(options, "job-name", "job-intlck-tpms");
    auto expected_artifact = std::filesystem::path{value_or(options, "expected", "")};
    if (expected_artifact.empty() &&
        operation == mbs::infrastructure::abaqus::AbaqusOperation::simulation) {
        expected_artifact = working_directory / (job_name + ".odb");
    } else if (expected_artifact.empty() &&
               operation == mbs::infrastructure::abaqus::AbaqusOperation::preprocess_validation) {
        expected_artifact = working_directory / (job_name + ".inp");
    } else if (expected_artifact.empty()) {
        throw std::invalid_argument{"--expected is required for postprocess and animation"};
    }
    const mbs::infrastructure::abaqus::AbaqusRequest request{
        .operation = operation,
        .command = value_or(options, "command", "abaqus"),
        .script = required(options, "script"),
        .config = required(options, "config"),
        .working_directory = working_directory,
        .job_name = job_name,
        .expected_artifact = expected_artifact,
        .target_step_time = positive_double(options, "step-time", 1.0),
        .monitor = operation == mbs::infrastructure::abaqus::AbaqusOperation::simulation,
    };
    const auto run_id = value_or(options, "run-id", "");
    const auto sample_id = value_or(options, "sample-id", "");
    active_cancellation = &cancellation;
#ifdef _WIN32
    static_cast<void>(SetConsoleCtrlHandler(console_handler, TRUE));
#endif
    try {
        const auto result = gateway.run(request, cancellation, [&](const auto& gateway_event) {
            if (gateway_event.kind == "log") {
                std::cout << gateway_event.message << '\n' << std::flush;
                return;
            }
            if (gateway_event.kind == "completed") {
                return;
            }
            emit({.event = gateway_event.kind,
                  .task_id = task_id,
                  .run_id = run_id,
                  .task_kind = operation_text,
                  .sample_id = sample_id,
                  .message = gateway_event.message,
                  .progress = gateway_event.progress,
                  .artifact_uris = {},
                  .proof_stress = std::nullopt,
                  .result_json = {}});
        });
        std::map<std::string, std::string, std::less<>> artifacts;
        std::optional<double> proof_stress;
        std::string result_json;
        if (!result.artifact.empty()) {
            const auto key =
                operation == mbs::infrastructure::abaqus::AbaqusOperation::simulation ? "odb_path"
                : operation == mbs::infrastructure::abaqus::AbaqusOperation::postprocess
                    ? "result_path"
                : operation == mbs::infrastructure::abaqus::AbaqusOperation::animation
                    ? "animation_manifest"
                    : "path";
            artifacts.emplace(key, result.artifact.generic_string());
            if (operation == mbs::infrastructure::abaqus::AbaqusOperation::postprocess) {
                result_json = read_text_file(result.artifact);
                proof_stress = json_number(result_json, "proof_stress");
                if (!proof_stress.has_value() || *proof_stress < 0.0) {
                    throw std::runtime_error{
                        "Abaqus postprocess result has no finite non-negative proof_stress"};
                }
            }
        }
        emit({.event = "completed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = operation_text,
              .sample_id = sample_id,
              .message = "Abaqus worker completed",
              .progress = 1.0,
              .artifact_uris = std::move(artifacts),
              .proof_stress = proof_stress,
              .result_json = std::move(result_json)});
        active_cancellation = nullptr;
        return 0;
    } catch (const std::exception& error) {
        const bool cancelled = cancellation.requested();
        emit({.event = "failed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = operation_text,
              .sample_id = sample_id,
              .message = cancelled ? "Cancelled by user" : error.what(),
              .progress = std::nullopt,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        active_cancellation = nullptr;
        return cancelled ? 130 : 1;
    }
}

int run_geometry(const Options& options) {
    const auto task_id = required(options, "task-id");
    const auto run_id = value_or(options, "run-id", "");
    const auto sample_id = value_or(options, "sample-id", "");
    mbs::domain::DesignConfig config;
    config.parameters.lambda = finite_double(options, "lambda", 0.24);
    config.parameters.mu = finite_double(options, "mu", 0.32);
    config.parameters.kappa = finite_double(options, "kappa", 0.58);
    config.parameters.beta = finite_double(options, "beta", 0.10);
    config.parameters.phase_x = finite_double(options, "phase-x", 0.0);
    config.parameters.phase_y = finite_double(options, "phase-y", 0.0);
    config.mesh.width = positive_double(options, "width", 10.0);
    config.mesh.repeat_z = integer_value(options, "repeat-z", 3);
    config.mesh.plate_thickness = positive_double(options, "plate-thickness", 1.0);
    config.mesh.resolution = integer_value(options, "resolution", 30);
    config.mesh.target_edge_percent = positive_double(options, "edge-percent", 5.0);
    config.mesh.sizing_mode =
        value_or(options, "sizing-mode", "curvature_adaptive") == "curvature_adaptive"
            ? mbs::domain::SurfaceSizingMode::curvature_adaptive
            : mbs::domain::SurfaceSizingMode::uniform;
    config.mesh.surface_tolerance_percent =
        positive_double(options, "surface-tolerance-percent", 0.5);
    config.mesh.minimum_edge_percent = positive_double(options, "minimum-edge-percent", 1.0);
    config.mesh.maximum_edge_percent = positive_double(options, "maximum-edge-percent", 5.0);
    config.mesh.remesh_iterations = integer_value(options, "remesh-iterations", 3);
    config.mesh.feature_angle_degrees =
        positive_double(options, "feature-angle", 30.0);
    config.mesh.sharpen = boolean_value(
        options, "sharpen", boolean_value(options, "preserve-features", false));
    config.mesh.simplify = boolean_value(options, "simplify", false);
    config.mesh.simplify_keep_ratio =
        finite_double(options, "simplify-keep-ratio", 0.8);
    config.mesh.repair_rounds = integer_value(options, "repair-rounds", 2);
    config.mesh.max_attempts = integer_value(options, "max-attempts", 20);
    config.mesh.tetrahedralize = boolean_value(options, "tetrahedralize", true);
    config.mesh.tetgen.order = integer_value(options, "tet-order", 1);
    config.mesh.tetgen.minimum_dihedral =
        finite_double(options, "tet-min-dihedral", 20.0);
    config.mesh.tetgen.minimum_ratio = finite_double(options, "tet-min-ratio", 1.1);
    config.mesh.tetgen.target_edge_length_mm =
        finite_double(options, "tet-target-edge", 0.0);
    config.mesh.tetgen.optimization_level = integer_value(options, "tet-optimization", 2);
    config.mesh.tetgen.no_bisect = boolean_value(options, "tet-no-bisect", true);
    config.mesh.tetgen.quality = boolean_value(options, "tet-quality", true);
    config.random_phase = boolean_value(options, "random-phase", true);
    config.part_b_construction =
        value_or(options, "part-b-construction", "shared_implicit_phase") ==
                "container_minus_part_a"
            ? mbs::domain::PartBConstruction::container_minus_part_a
            : mbs::domain::PartBConstruction::shared_implicit_phase;
    config.output_directory = required(options, "output-dir");
    config.sample_id = sample_id;

    try {
        mbs::geometry::GeometryKernel kernel;
        const auto result = kernel.generate(
            config, {.progress = [&](const double progress, const std::string_view message) {
                         emit({.event = "progress",
                               .task_id = task_id,
                               .run_id = run_id,
                               .task_kind = "geometry",
                               .sample_id = sample_id,
                               .message = std::string{message},
                               .progress = progress,
                               .artifact_uris = {},
                               .proof_stress = std::nullopt,
                               .result_json = {}});
                     },
                     .cancelled = {}});
        const auto published = mbs::geometry::publish_geometry(
            result, config, std::filesystem::path{config.output_directory});
        std::map<std::string, std::string, std::less<>> artifacts{
            {"mesh_dir", published.directory.generic_string()},
            {"tri_a", (published.directory / "tpms-tri-A.obj").generic_string()},
            {"tri_b", (published.directory / "tpms-tri-B.obj").generic_string()},
            {"metadata", (published.directory / "mesh_metadata.json").generic_string()}};
        if (config.mesh.tetrahedralize) {
            artifacts.emplace("tet_a", (published.directory / "tpms-tet-A.inp").generic_string());
            artifacts.emplace("tet_b", (published.directory / "tpms-tet-B.inp").generic_string());
        }
        emit({.event = "completed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "geometry",
              .sample_id = sample_id,
              .message = "C++ TPMS geometry completed",
              .progress = 1.0,
              .artifact_uris = std::move(artifacts),
              .proof_stress = std::nullopt,
              .result_json = mbs::geometry::metrics_json(result)});
        return 0;
    } catch (const std::exception& error) {
        emit({.event = "failed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "geometry",
              .sample_id = sample_id,
              .message = error.what(),
              .progress = std::nullopt,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        return 1;
    }
}

int run_contact_risk(const Options& options) {
    const auto task_id = required(options, "task-id");
    const auto run_id = value_or(options, "run-id", "");
    const auto sample_id = value_or(options, "sample-id", "");
    const auto mesh_directory = std::filesystem::path{required(options, "mesh-dir")};
    try {
        emit({.event = "progress",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "contact-risk",
              .sample_id = sample_id,
              .message = "Reading C3D4/C3D10 meshes and extracting contact surfaces",
              .progress = 0.1,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        const auto report = mbs::geometry::analyze_contact_risk(
            mesh_directory, positive_double(options, "boundary-tolerance", 0.01),
            positive_double(options, "small-volume", 1.0e-9));
        const auto report_path = mesh_directory / "contact_risk_report.json";
        mbs::geometry::save_contact_risk(report, report_path);
        std::cout << report.format() << '\n' << std::flush;
        emit({.event = "completed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "contact-risk",
              .sample_id = sample_id,
              .message = "C++ contact-risk analysis completed",
              .progress = 1.0,
              .artifact_uris = {{"contact_risk_report", report_path.generic_string()}},
              .proof_stress = std::nullopt,
              .result_json = report.to_json()});
        return 0;
    } catch (const std::exception& error) {
        emit({.event = "failed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "contact-risk",
              .sample_id = sample_id,
              .message = error.what(),
              .progress = std::nullopt,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        return 1;
    }
}

int run_optimization(const Options& options) {
    const auto task_id = required(options, "task-id");
    const auto run_id = value_or(options, "run-id", "");
    try {
        mbs::domain::BayesianOptimizationConfig config;
        config.acquisition = acquisition_from(value_or(options, "acquisition", "EI"));
        config.initial_points = integer_value(options, "initial-points", 10);
        const auto seed = integer_value(options, "random-seed", 0);
        if (seed < 0) {
            throw std::invalid_argument{"--random-seed must be non-negative"};
        }
        config.random_seed = static_cast<std::uint64_t>(seed);
        config.candidate_pool = integer_value(options, "candidate-pool", 16);
        config.kappa = finite_double(options, "kappa", 1.96);
        config.xi = finite_double(options, "xi", 0.01);
        mbs::domain::require_valid(config.validation_errors());

        mbs::infrastructure::sqlite::SqliteOptimizationStateRepository repository{
            std::filesystem::path{required(options, "database")},
            value_or(options, "project-id", "default")};
        const auto observations = repository.observations();
        emit({.event = "progress",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "optimization",
              .sample_id = {},
              .message = "Fitting native C++ Gaussian-process optimizer",
              .progress = 0.2,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        mbs::optimization::GaussianProcessOptimizer optimizer;
        const auto proposal = optimizer.propose_detailed(config, observations);
        const auto payload = proposal.to_json();
        repository.save_pending_json(payload);
        emit({.event = "completed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "optimization",
              .sample_id = {},
              .message = "Native C++ Bayesian candidate saved to SQLite",
              .progress = 1.0,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = payload});
        return 0;
    } catch (const std::exception& error) {
        emit({.event = "failed",
              .task_id = task_id,
              .run_id = run_id,
              .task_kind = "optimization",
              .sample_id = {},
              .message = error.what(),
              .progress = std::nullopt,
              .artifact_uris = {},
              .proof_stress = std::nullopt,
              .result_json = {}});
        return 1;
    }
}

void print_usage() {
    std::cerr << "Usage:\n"
              << "  mbs-worker\n"
              << "  mbs-worker geometry --task-id ID --output-dir DIR [design options]\n"
              << "  mbs-worker contact-risk --task-id ID --mesh-dir DIR\n"
              << "  mbs-worker optimize --task-id ID --database FILE [BO options]\n"
              << "  mbs-worker abaqus --operation NAME --task-id ID --run-id ID --script FILE "
                 "--config FILE --work-dir DIR [--command abaqus] [--job-name NAME] "
                 "[--expected FILE] [--step-time SECONDS]\n";
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 1) {
            health_check();
            return 0;
        }
        if (std::string_view{argv[1]} == "abaqus") {
            return run_abaqus(parse_options(argc, argv, 2));
        }
        if (std::string_view{argv[1]} == "geometry") {
            return run_geometry(parse_options(argc, argv, 2));
        }
        if (std::string_view{argv[1]} == "contact-risk") {
            return run_contact_risk(parse_options(argc, argv, 2));
        }
        if (std::string_view{argv[1]} == "optimize") {
            return run_optimization(parse_options(argc, argv, 2));
        }
        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "mbs-worker: " << error.what() << '\n';
        print_usage();
        return 2;
    }
}
