#pragma once

#include "mbs/geometry/GeometryKernel.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::geometry {

struct SurfaceDefects final {
    std::size_t boundary_edges{};
    std::size_t connected_components{};
    std::size_t incident_faces_on_non_manifold_edges{};
    std::size_t incident_faces_on_non_manifold_vertices{};
    bool two_manifold{};
    std::size_t non_manifold_edges{};
    std::size_t non_manifold_vertices{};
    std::size_t holes{};
    std::size_t unreferenced_vertices{};
    std::size_t degenerate_faces{};
    std::size_t duplicate_faces{};
    std::size_t self_intersection_pairs{};
    std::size_t self_intersecting_faces{};

    [[nodiscard]] bool acceptable_for_volume_meshing() const noexcept;
    [[nodiscard]] std::vector<std::string> readable_lines(std::string_view part) const;
};

struct MeshRepairResult final {
    SurfaceMesh mesh;
    SurfaceDefects before;
    SurfaceDefects after;
    bool changed{};
    bool accepted{};
    std::string backend;
    std::string detail;
    double volume_relative_change{};
};

struct RemeshResult final {
    SurfaceMesh mesh;
    bool changed{};
    std::size_t sharp_edges{};
    double target_edge_length{};
    double minimum_angle_degrees{};
    double mean_aspect_ratio{};
    double maximum_aspect_ratio{};
    double minimum_edge_length{};
    double mean_edge_length{};
    double maximum_edge_length{};
    double edge_length_cv{};
    double surface_deviation{};
    bool simplified{};
    bool simplification_rolled_back{};
    std::string backend;
    std::string detail;
};

struct SurfaceRemeshOptions final {
    domain::SurfaceSizingMode sizing_mode{domain::SurfaceSizingMode::curvature_adaptive};
    double target_edge_length{};
    double surface_tolerance{};
    double minimum_edge_length{};
    double maximum_edge_length{};
    int iterations{3};
    double feature_angle_degrees{30.0};
    bool sharpen{};
    bool simplify{};
    double simplify_keep_ratio{0.8};
};

[[nodiscard]] SurfaceDefects inspect_surface(const SurfaceMesh& mesh);
[[nodiscard]] MeshRepairResult repair_surface(const SurfaceMesh& mesh,
                                              std::size_t maximum_rounds = 2);
[[nodiscard]] RemeshResult isotropic_remesh(const SurfaceMesh& mesh,
                                            double target_edge_length,
                                            int iterations,
                                            double feature_angle_degrees,
                                            bool preserve_sharp_features = true);
[[nodiscard]] RemeshResult remesh_surface(const SurfaceMesh& mesh,
                                          const SurfaceRemeshOptions& options);

} // namespace mbs::geometry
