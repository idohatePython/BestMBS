#include "mbs/domain/DesignParameters.hpp"

#include <cmath>

namespace mbs::domain {
namespace {

constexpr double epsilon = 1.0e-12;

void require_range(std::vector<std::string>& errors, const double value, const double minimum,
                   const double maximum, const char* message) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        errors.emplace_back(message);
    }
}

} // namespace

ValidationErrors DesignParameters::validation_errors() const {
    ValidationErrors errors;
    require_range(errors, lambda, 0.0, 1.0, "lambda must be in [0, 1]");
    if (!std::isfinite(mu) || mu < 0.0 || mu > 1.0 - lambda + epsilon) {
        errors.emplace_back("mu must satisfy 0 <= mu <= 1 - lambda");
    }
    require_range(errors, kappa, 0.0, 1.0, "kappa must be in [0, 1]");
    require_range(errors, beta, -1.0, 1.0, "beta must be in [-1, 1]");
    return errors;
}

bool DesignParameters::is_valid() const { return validation_errors().empty(); }

ValidationErrors TetgenSettings::validation_errors() const {
    ValidationErrors errors;
    if (order != 1 && order != 2) {
        errors.emplace_back("tetgen order must be 1 (Tet4) or 2 (Tet10)");
    }
    if (!std::isfinite(minimum_dihedral) || minimum_dihedral < 0.0 || minimum_dihedral >= 90.0) {
        errors.emplace_back("minimum_dihedral must be in [0, 90)");
    }
    if (!std::isfinite(minimum_ratio) || minimum_ratio <= 1.0) {
        errors.emplace_back("minimum_ratio must be greater than 1");
    }
    if (!std::isfinite(target_edge_length_mm) || target_edge_length_mm < 0.0 ||
        target_edge_length_mm > 100.0) {
        errors.emplace_back("target_edge_length_mm must be in [0, 100]");
    }
    if (optimization_level < 0 || optimization_level > 10) {
        errors.emplace_back("optimization_level must be in [0, 10]");
    }
    return errors;
}

bool TetgenSettings::is_valid() const { return validation_errors().empty(); }

ValidationErrors MeshSettings::validation_errors() const {
    ValidationErrors errors;
    require_range(errors, width, 1.0, 100.0, "width must be in [1, 100]");
    if (repeat_z < 1 || repeat_z > 10) {
        errors.emplace_back("repeat_z must be in [1, 10]");
    }
    require_range(errors, plate_thickness, 0.1, 20.0, "plate_thickness must be in [0.1, 20]");
    if (resolution < 20 || resolution > 200) {
        errors.emplace_back("resolution must be in [20, 200]");
    }
    require_range(errors, target_edge_percent, 0.1, 10.0,
                  "target_edge_percent must be in [0.1, 10]");
    require_range(errors, surface_tolerance_percent, 0.01, 5.0,
                  "surface_tolerance_percent must be in [0.01, 5]");
    require_range(errors, minimum_edge_percent, 0.05, 10.0,
                  "minimum_edge_percent must be in [0.05, 10]");
    require_range(errors, maximum_edge_percent, 0.05, 20.0,
                  "maximum_edge_percent must be in [0.05, 20]");
    if (minimum_edge_percent > maximum_edge_percent) {
        errors.emplace_back("minimum_edge_percent must not exceed maximum_edge_percent");
    }
    if (remesh_iterations < 1 || remesh_iterations > 10) {
        errors.emplace_back("remesh_iterations must be in [1, 10]");
    }
    require_range(errors, feature_angle_degrees, 1.0, 90.0,
                  "feature_angle_degrees must be in [1, 90]");
    require_range(errors, simplify_keep_ratio, 0.1, 1.0,
                  "simplify_keep_ratio must be in [0.1, 1]");
    if (repair_rounds < 0 || repair_rounds > 5) {
        errors.emplace_back("repair_rounds must be in [0, 5]");
    }
    if (max_attempts < 1 || max_attempts > 100) {
        errors.emplace_back("max_attempts must be in [1, 100]");
    }
    auto tetgen_errors = tetgen.validation_errors();
    errors.insert(errors.end(), tetgen_errors.begin(), tetgen_errors.end());
    return errors;
}

bool MeshSettings::is_valid() const { return validation_errors().empty(); }

std::size_t MeshSettings::estimated_grid_points() const {
    const auto xy = static_cast<std::size_t>(2.25 * static_cast<double>(resolution) + 1.0);
    const auto z =
        static_cast<std::size_t>((static_cast<double>(repeat_z) + 4.0 * plate_thickness / width) *
                                     static_cast<double>(resolution) +
                                 1.0);
    return xy * xy * z;
}

std::uint64_t MeshSettings::estimated_memory_bytes(const std::uint64_t bytes_per_point) const {
    return static_cast<std::uint64_t>(estimated_grid_points()) * bytes_per_point;
}

ValidationErrors DesignConfig::validation_errors() const {
    auto errors = parameters.validation_errors();
    auto mesh_errors = mesh.validation_errors();
    errors.insert(errors.end(), mesh_errors.begin(), mesh_errors.end());
    if (!random_phase) {
        require_range(errors, parameters.phase_x, 0.0, 1.0, "phase_x must be in [0, 1]");
        require_range(errors, parameters.phase_y, 0.0, 1.0, "phase_y must be in [0, 1]");
    }
    return errors;
}

bool DesignConfig::is_valid() const { return validation_errors().empty(); }

std::string_view to_string(const DesignSource source) noexcept {
    switch (source) {
    case DesignSource::manual:
        return "manual";
    case DesignSource::bayesian_optimization:
        return "bo";
    case DesignSource::existing:
        return "existing";
    }
    return "unknown";
}

std::string_view to_string(const SurfaceSizingMode mode) noexcept {
    switch (mode) {
    case SurfaceSizingMode::uniform:
        return "uniform";
    case SurfaceSizingMode::curvature_adaptive:
        return "curvature_adaptive";
    }
    return "unknown";
}

std::string_view to_string(const PartBConstruction mode) noexcept {
    switch (mode) {
    case PartBConstruction::shared_implicit_phase: return "shared_implicit_phase";
    case PartBConstruction::container_minus_part_a: return "container_minus_part_a";
    }
    return "shared_implicit_phase";
}

} // namespace mbs::domain
