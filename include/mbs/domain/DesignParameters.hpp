#pragma once

#include "mbs/domain/Validation.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mbs::domain {

enum class SurfaceSizingMode { uniform, curvature_adaptive };
enum class PartBConstruction { shared_implicit_phase, container_minus_part_a };

struct DesignParameters final {
    double lambda{};
    double mu{};
    double kappa{};
    double beta{};
    double phase_x{};
    double phase_y{};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct TetgenSettings final {
    int order{1};
    double minimum_dihedral{20.0};
    double minimum_ratio{1.1};
    double target_edge_length_mm{};
    int optimization_level{2};
    bool no_bisect{true};
    bool quality{true};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct MeshSettings final {
    double width{10.0};
    int repeat_z{3};
    double plate_thickness{1.0};
    int resolution{30};
    double target_edge_percent{5.0};
    SurfaceSizingMode sizing_mode{SurfaceSizingMode::curvature_adaptive};
    double surface_tolerance_percent{0.5};
    double minimum_edge_percent{1.0};
    double maximum_edge_percent{5.0};
    int remesh_iterations{3};
    double feature_angle_degrees{30.0};
    bool sharpen{false};
    bool simplify{false};
    double simplify_keep_ratio{0.8};
    int repair_rounds{2};
    int max_attempts{20};
    bool tetrahedralize{true};
    TetgenSettings tetgen{};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] std::size_t estimated_grid_points() const;
    [[nodiscard]] std::uint64_t estimated_memory_bytes(std::uint64_t bytes_per_point = 192) const;
};

enum class DesignSource { manual, bayesian_optimization, existing };

struct DesignConfig final {
    DesignParameters parameters{};
    MeshSettings mesh{};
    DesignSource source{DesignSource::manual};
    bool random_phase{true};
    PartBConstruction part_b_construction{PartBConstruction::shared_implicit_phase};
    std::string output_directory;
    std::string sample_id;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

[[nodiscard]] std::string_view to_string(DesignSource source) noexcept;
[[nodiscard]] std::string_view to_string(SurfaceSizingMode mode) noexcept;
[[nodiscard]] std::string_view to_string(PartBConstruction mode) noexcept;

} // namespace mbs::domain
