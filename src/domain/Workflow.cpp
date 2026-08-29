#include "mbs/domain/Workflow.hpp"

#include <cmath>

namespace mbs::domain {
namespace {

void append(ValidationErrors& destination, ValidationErrors source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

bool blank(const std::string_view value) {
    return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

} // namespace

ValidationErrors ContactSettings::validation_errors() const {
    ValidationErrors errors;
    if (!std::isfinite(friction_coefficient) || friction_coefficient < 0.0) {
        errors.emplace_back("friction coefficient must be finite and non-negative");
    }
    if (formulation == ContactFormulation::frictionless &&
        std::abs(friction_coefficient) > 1.0e-12) {
        errors.emplace_back("frictionless contact requires a zero friction coefficient");
    }
    if (adjustment == ContactAdjustment::tolerance &&
        (!std::isfinite(adjustment_tolerance) || adjustment_tolerance <= 0.0)) {
        errors.emplace_back("adjustment tolerance must be positive");
    }
    return errors;
}

bool ContactSettings::is_valid() const { return validation_errors().empty(); }

ValidationErrors StepSettings::validation_errors() const {
    ValidationErrors errors;
    if (!std::isfinite(step_time) || step_time <= 0.0) {
        errors.emplace_back("step time must be positive");
    }
    if (!std::isfinite(minimum_increment) || !std::isfinite(initial_increment) ||
        !std::isfinite(maximum_increment) || minimum_increment <= 0.0 ||
        minimum_increment > initial_increment || initial_increment > maximum_increment ||
        maximum_increment > step_time) {
        errors.emplace_back("increments must satisfy 0 < min <= initial <= max <= step time");
    }
    if (maximum_increments < 1) {
        errors.emplace_back("maximum increments must be positive");
    }
    if (!std::isfinite(stabilization_magnitude) || stabilization_magnitude < 0.0 ||
        !std::isfinite(adaptive_damping_ratio) || adaptive_damping_ratio < 0.0) {
        errors.emplace_back("step damping settings must be finite and non-negative");
    }
    return errors;
}

bool StepSettings::is_valid() const { return validation_errors().empty(); }

ValidationErrors SolverResources::validation_errors() const {
    ValidationErrors errors;
    if (cpu_count < 1 || cpu_count > 16) {
        errors.emplace_back("CPU count must be in [1, 16]");
    }
    if (memory_percent < 1 || memory_percent > 100) {
        errors.emplace_back("memory percent must be in [1, 100]");
    }
    return errors;
}

bool SolverResources::is_valid() const { return validation_errors().empty(); }

ValidationErrors SimulationConfig::validation_errors() const {
    ValidationErrors errors;
    if (blank(mesh_directory) || blank(work_directory)) {
        errors.emplace_back("mesh and work directories are required");
    }
    if (blank(abaqus_command) || blank(job_name)) {
        errors.emplace_back("Abaqus command and job name are required");
    }
    if (!std::isfinite(width) || width <= 0.0 || repeat_z < 1 || !std::isfinite(plate_thickness) ||
        plate_thickness <= 0.0) {
        errors.emplace_back("width, repeat_z and plate thickness must be positive");
    }
    if (!std::isfinite(tensile_displacement) || tensile_displacement < 0.0 ||
        tensile_displacement > width * static_cast<double>(repeat_z)) {
        errors.emplace_back("tensile displacement must be in [0, width * repeat_z]");
    }
    if (use_target_mesh_size && (!std::isfinite(target_mesh_size) || target_mesh_size <= 0.0)) {
        errors.emplace_back("Abaqus target mesh size must be positive");
    }
    append(errors, resources.validation_errors());
    append(errors, step.validation_errors());
    append(errors, contact.validation_errors());
    append(errors, materials[0].validation_errors());
    append(errors, materials[1].validation_errors());
    return errors;
}

bool SimulationConfig::is_valid() const { return validation_errors().empty(); }

std::array<std::string_view, 2> SimulationConfig::required_mesh_files() const noexcept {
    if (backend == SimulationBackend::tetgen) {
        return {"tpms-tet-A.inp", "tpms-tet-B.inp"};
    }
    return {"tpms-tri-A.inp", "tpms-tri-B.inp"};
}

ValidationErrors PostprocessConfig::validation_errors() const {
    ValidationErrors errors;
    if (blank(odb_path) || blank(output_json)) {
        errors.emplace_back("ODB and output JSON paths are required");
    }
    if (!std::isfinite(width) || width <= 0.0 || repeat_z < 1) {
        errors.emplace_back("width and repeat_z must be positive");
    }
    return errors;
}

bool PostprocessConfig::is_valid() const { return validation_errors().empty(); }

ValidationErrors BayesianOptimizationConfig::validation_errors() const {
    ValidationErrors errors;
    if (initial_points < 0 || candidate_pool < 1) {
        errors.emplace_back("Bayesian optimizer counts are invalid");
    }
    if (!std::isfinite(kappa) || kappa < 0.0 || !std::isfinite(xi) || xi < 0.0) {
        errors.emplace_back("Bayesian acquisition parameters must be finite and non-negative");
    }
    return errors;
}

bool BayesianOptimizationConfig::is_valid() const { return validation_errors().empty(); }

ValidationErrors Sample::validation_errors() const {
    ValidationErrors errors;
    if (blank(id) || blank(project_id)) {
        errors.emplace_back("sample and project identifiers are required");
    }
    if (serial < 1) {
        errors.emplace_back("sample serial must be positive");
    }
    append(errors, parameters.validation_errors());
    append(errors, mesh.validation_errors());
    return errors;
}

bool Sample::is_valid() const { return validation_errors().empty(); }

ValidationErrors Task::validation_errors() const {
    ValidationErrors errors;
    if (blank(id) || blank(kind)) {
        errors.emplace_back("task id and kind are required");
    }
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
        errors.emplace_back("Task progress must be in [0, 1]");
    }
    if (status == TaskStatus::succeeded && std::abs(progress - 1.0) > 1.0e-12) {
        errors.emplace_back("a succeeded task must have progress 1");
    }
    return errors;
}

bool Task::is_valid() const { return validation_errors().empty(); }

bool is_terminal(const TaskStatus status) noexcept {
    return status == TaskStatus::succeeded || status == TaskStatus::failed ||
           status == TaskStatus::cancelled || status == TaskStatus::interrupted;
}

bool transition_allowed(const TaskStatus from, const TaskStatus to) noexcept {
    if (is_terminal(from)) {
        return false;
    }
    if (from == TaskStatus::queued) {
        return to == TaskStatus::running || to == TaskStatus::cancelled ||
               to == TaskStatus::interrupted;
    }
    return from == TaskStatus::running && (to == TaskStatus::running || is_terminal(to));
}

std::string_view to_string(const TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::queued:
        return "queued";
    case TaskStatus::running:
        return "running";
    case TaskStatus::succeeded:
        return "succeeded";
    case TaskStatus::failed:
        return "failed";
    case TaskStatus::cancelled:
        return "cancelled";
    case TaskStatus::interrupted:
        return "interrupted";
    }
    return "unknown";
}

std::string_view to_string(const DatasetKind dataset) noexcept {
    return dataset == DatasetKind::mbs ? "mbs" : "demo";
}

std::string_view to_string(const SimulationBackend backend) noexcept {
    return backend == SimulationBackend::tetgen ? "tetgen" : "abaqus";
}

std::string_view to_string(const AcquisitionFunction acquisition) noexcept {
    switch (acquisition) {
    case AcquisitionFunction::lower_confidence_bound:
        return "LCB";
    case AcquisitionFunction::expected_improvement:
        return "EI";
    case AcquisitionFunction::probability_improvement:
        return "PI";
    }
    return "unknown";
}

} // namespace mbs::domain
