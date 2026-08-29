#pragma once

#include "mbs/domain/DesignParameters.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::geometry {

struct SurfaceMesh final {
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

struct TetrahedralMesh final {
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<std::uint32_t, 10>> tetrahedra;
    int order{1};

    [[nodiscard]] std::size_t nodes_per_element() const noexcept {
        return order == 2 ? 10U : 4U;
    }
};

struct SurfaceMetrics final {
    std::size_t points{};
    std::size_t triangles{};
    std::size_t connected_components{};
    int genus{};
    double area{};
    double volume{};
    std::array<double, 6> bounds{};
    bool closed{};
    bool two_manifold{};
    std::size_t boundary_edges{};
    std::size_t incident_faces_on_non_manifold_edges{};
    std::size_t incident_faces_on_non_manifold_vertices{};
    std::size_t non_manifold_edges{};
    std::size_t non_manifold_vertices{};
    std::size_t holes{};
    std::size_t unreferenced_vertices{};
    std::size_t degenerate_faces{};
    std::size_t duplicate_faces{};
    std::size_t self_intersection_pairs{};
    std::size_t self_intersecting_faces{};
    double target_edge_length{};
    double minimum_triangle_angle_degrees{};
    double mean_triangle_aspect_ratio{};
    double maximum_triangle_aspect_ratio{};
    double edge_length_coefficient_of_variation{};
    std::size_t protected_sharp_edges{};
    double surface_deviation{};
    bool simplified{};
    bool simplification_rolled_back{};
};

struct TetrahedralMetrics final {
    std::size_t points{};
    std::size_t tetrahedra{};
    double minimum_volume{};
    double minimum_signed_volume{};
    std::size_t non_positive_count{};
    std::size_t small_volume_count{};
};

struct PartGeometry final {
    SurfaceMesh raw_surface;
    SurfaceMesh plate_union_surface;
    SurfaceMesh boolean_surface;
    SurfaceMesh surface;
    SurfaceMetrics surface_metrics;
    std::optional<TetrahedralMesh> volume;
    std::optional<TetrahedralMetrics> volume_metrics;
    bool surface_repaired{};
    std::string repair_backend;
    double repair_volume_relative_change{};
};

struct GeometryResult final {
    PartGeometry part_a;
    PartGeometry part_b;
    double phase_x{};
    double phase_y{};
    double grid_spacing{};
    std::string surface_backend{"manifold-level-set"};
    std::string tetrahedral_backend;
};

struct GeometryCallbacks final {
    std::function<void(double, std::string_view)> progress;
    std::function<bool()> cancelled;
};

class GeometryKernel final {
  public:
    [[nodiscard]] static double implicit_field(double x, double y, double z,
                                               const domain::DesignParameters& parameters,
                                               double scale);

    [[nodiscard]] GeometryResult generate(const domain::DesignConfig& config,
                                          const GeometryCallbacks& callbacks = {}) const;
};

struct PublishedGeometry final {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;
};

[[nodiscard]] PublishedGeometry publish_geometry(const GeometryResult& result,
                                                 const domain::DesignConfig& config,
                                                 const std::filesystem::path& output_directory);

[[nodiscard]] std::string metrics_json(const GeometryResult& result);

} // namespace mbs::geometry
