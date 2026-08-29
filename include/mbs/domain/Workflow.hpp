#pragma once

#include "mbs/domain/DesignParameters.hpp"
#include "mbs/domain/Material.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::domain {

using Properties = std::map<std::string, std::string, std::less<>>;

enum class DatasetKind { mbs, demo };
enum class SimulationBackend { tetgen, abaqus };
enum class ContactFormulation { frictionless, penalty };
enum class SlidingFormulation { small, finite };
enum class ContactAdjustment { none, overclosed, tolerance };
enum class TaskStatus { queued, running, succeeded, failed, cancelled, interrupted };
enum class AcquisitionFunction {
    lower_confidence_bound,
    expected_improvement,
    probability_improvement
};

struct ContactSettings final {
    ContactFormulation formulation{ContactFormulation::frictionless};
    double friction_coefficient{};
    SlidingFormulation sliding{SlidingFormulation::small};
    ContactAdjustment adjustment{ContactAdjustment::overclosed};
    double adjustment_tolerance{};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct StepSettings final {
    double step_time{1.0};
    double stabilization_magnitude{0.0002};
    double adaptive_damping_ratio{0.05};
    int maximum_increments{500};
    double initial_increment{0.001};
    double minimum_increment{1.0e-5};
    double maximum_increment{0.1};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct SolverResources final {
    int cpu_count{10};
    int memory_percent{90};
    std::string scratch_directory;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct SimulationConfig final {
    std::string sample_id;
    std::string mesh_directory;
    std::string work_directory;
    SimulationBackend backend{SimulationBackend::tetgen};
    std::string abaqus_command{"abaqus"};
    std::string job_name{"job-intlck-tpms"};
    double width{10.0};
    int repeat_z{3};
    double plate_thickness{1.0};
    double tensile_displacement{2.0};
    SolverResources resources{};
    StepSettings step{};
    ContactSettings contact{};
    std::array<MaterialDefinition, 2> materials{default_material_pair()};
    bool use_target_mesh_size{true};
    double target_mesh_size{0.6};
    Properties options;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] std::array<std::string_view, 2> required_mesh_files() const noexcept;
};

struct PostprocessConfig final {
    std::string sample_id;
    std::string odb_path;
    std::string output_json;
    double width{10.0};
    int repeat_z{3};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct BayesianOptimizationConfig final {
    AcquisitionFunction acquisition{AcquisitionFunction::expected_improvement};
    int initial_points{10};
    std::uint64_t random_seed{};
    int candidate_pool{16};
    double kappa{1.96};
    double xi{0.01};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct Sample final {
    std::string id;
    std::string project_id;
    DatasetKind dataset{DatasetKind::mbs};
    int serial{};
    DesignParameters parameters{};
    MeshSettings mesh{};
    DesignSource source{DesignSource::manual};
    std::string status;
    std::optional<std::string> artifact_directory;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct Run final {
    std::string id;
    std::string kind;
    TaskStatus status{TaskStatus::queued};
    std::optional<std::string> sample_id;
    std::string request_json;
    std::string error;
};

struct Task final {
    std::string id;
    std::string kind;
    TaskStatus status{TaskStatus::queued};
    std::optional<std::string> sample_id;
    std::optional<std::string> run_id;
    double progress{};
    std::string error;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct ArtifactRef final {
    std::string id;
    std::string kind;
    std::string uri;
    std::optional<std::string> sample_id;
    std::optional<std::string> run_id;
    std::optional<std::uint64_t> size_bytes;
    std::optional<std::string> checksum;
};

struct Metric final {
    std::string id;
    std::string name;
    double value{};
    std::string unit;
    std::optional<std::string> sample_id;
    std::optional<std::string> run_id;
    std::string details_json;
};

struct OptimizationObservation final {
    std::string id;
    std::string sample_id;
    double objective{};
    DesignParameters parameters{};
};

[[nodiscard]] bool is_terminal(TaskStatus status) noexcept;
[[nodiscard]] bool transition_allowed(TaskStatus from, TaskStatus to) noexcept;
[[nodiscard]] std::string_view to_string(TaskStatus status) noexcept;
[[nodiscard]] std::string_view to_string(DatasetKind dataset) noexcept;
[[nodiscard]] std::string_view to_string(SimulationBackend backend) noexcept;
[[nodiscard]] std::string_view to_string(AcquisitionFunction acquisition) noexcept;

} // namespace mbs::domain
