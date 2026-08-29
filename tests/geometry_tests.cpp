#include "TestSupport.hpp"
#include "mbs/geometry/ContactRisk.hpp"
#include "mbs/geometry/GeometryKernel.hpp"
#include "mbs/geometry/MeshDoctor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

namespace {

bool close(const double actual, const double expected, const double relative,
           const double absolute = 1.0e-12) {
    return std::abs(actual - expected) <=
           std::max(absolute, relative * std::max(std::abs(actual), std::abs(expected)));
}

} // namespace

int main() {
    int failures = 0;
    using mbs::domain::DesignParameters;
    using mbs::geometry::GeometryKernel;

    MBS_CHECK(close(GeometryKernel::implicit_field(0.0, 0.0, 0.0, {},
                                                    1.5915494309189535),
                    0.0, 1.0e-10));
    MBS_CHECK(close(GeometryKernel::implicit_field(
                        1.25, -0.75, 2.5,
                        DesignParameters{.lambda = 0.35,
                                         .mu = 0.25,
                                         .kappa = 0.4,
                                         .beta = -0.15},
                        1.5915494309189535),
                    0.940809781718, 1.0e-10));
    MBS_CHECK(close(GeometryKernel::implicit_field(
                        -3.2, 4.1, -1.7,
                        DesignParameters{.lambda = 0.6,
                                         .mu = 0.1,
                                         .kappa = 0.85,
                                         .beta = 0.3},
                        2.25),
                    -0.072004506574, 1.0e-10));
    MBS_CHECK(close(GeometryKernel::implicit_field(
                        2.0 * std::numbers::pi, std::numbers::pi,
                        std::numbers::pi / 2.0,
                        DesignParameters{.lambda = 1.0,
                                         .mu = 0.0,
                                         .kappa = 0.0,
                                         .beta = -1.0},
                        1.0),
                    0.0, 1.0e-10));

    const mbs::geometry::SurfaceMesh crossing_triangles{
        .points = {{{-1.0, -1.0, 0.0}},
                   {{1.0, -1.0, 0.0}},
                   {{0.0, 1.0, 0.0}},
                   {{0.0, -0.5, -1.0}},
                   {{0.0, -0.5, 1.0}},
                   {{0.0, 0.75, 0.0}}},
        .triangles = {{{0, 1, 2}}, {{3, 4, 5}}}};
    const auto crossing_defects = mbs::geometry::inspect_surface(crossing_triangles);
    MBS_CHECK(crossing_defects.self_intersection_pairs == 1);
    MBS_CHECK(crossing_defects.self_intersecting_faces == 2);
    MBS_CHECK(!crossing_defects.acceptable_for_volume_meshing());
    const auto readable = crossing_defects.readable_lines("测试网格");
    MBS_CHECK(std::ranges::any_of(readable, [](const std::string& line) {
        return line.find("自相交面对") != std::string::npos;
    }));

    mbs::domain::DesignConfig config;
    config.parameters = {.lambda = 0.35,
                         .mu = 0.25,
                         .kappa = 0.4,
                         .beta = -0.15,
                         .phase_x = 0.125,
                         .phase_y = 0.375};
    config.mesh.width = 10.0;
    config.mesh.repeat_z = 1;
    config.mesh.plate_thickness = 1.0;
    // Exercise the shipped sampling default.  The enlarged 3.0-compatible
    // Boolean construction needs the production sampling density here; the
    // minimum validation-only density of 20 is intentionally much coarser.
    config.mesh.resolution = 30;
    config.mesh.target_edge_percent = 5.0;
    config.mesh.max_attempts = 2;
#ifdef MBS_TEST_TETGEN
    config.mesh.tetrahedralize = true;
#else
    config.mesh.tetrahedralize = false;
#endif
    config.random_phase = false;

    const auto result = GeometryKernel{}.generate(config);
    const auto& part_a = result.part_a.surface_metrics;
    const auto& part_b = result.part_b.surface_metrics;
    MBS_CHECK(part_a.closed && part_a.two_manifold && part_a.connected_components == 1);
    MBS_CHECK(part_b.closed && part_b.two_manifold && part_b.connected_components == 1);
    for (const auto* metrics : {&part_a, &part_b}) {
        MBS_CHECK(metrics->boundary_edges == 0);
        MBS_CHECK(metrics->non_manifold_edges == 0);
        MBS_CHECK(metrics->non_manifold_vertices == 0);
        MBS_CHECK(metrics->holes == 0);
        MBS_CHECK(metrics->unreferenced_vertices == 0);
        MBS_CHECK(metrics->degenerate_faces == 0);
        MBS_CHECK(metrics->duplicate_faces == 0);
        MBS_CHECK(metrics->self_intersection_pairs == 0);
        MBS_CHECK(metrics->self_intersecting_faces == 0);
        MBS_CHECK(metrics->minimum_triangle_angle_degrees >= 5.0);
        MBS_CHECK(metrics->mean_triangle_aspect_ratio >= 1.0);
        MBS_CHECK(metrics->mean_triangle_aspect_ratio <= 2.0);
        MBS_CHECK(metrics->maximum_triangle_aspect_ratio <= 10.0);
        MBS_CHECK(metrics->edge_length_coefficient_of_variation <= 0.35);
    }
    MBS_CHECK(close(part_a.area, 561.129925456787, 0.03));
    MBS_CHECK(close(part_b.area, 474.091267408149, 0.03));
    MBS_CHECK(close(part_a.volume, 647.343968968618, 0.03));
    MBS_CHECK(close(part_b.volume, 552.644313332269, 0.03));
    const std::array expected_a{-5.0, 5.0, -5.0, 5.0, -6.0, 4.121183395386};
    const std::array expected_b{-5.0, 5.0, -5.0, 5.0, -2.392418384552, 6.0};
    for (std::size_t index = 0; index < expected_a.size(); ++index) {
        MBS_CHECK(std::abs(part_a.bounds[index] - expected_a[index]) <= result.grid_spacing);
        MBS_CHECK(std::abs(part_b.bounds[index] - expected_b[index]) <= result.grid_spacing);
    }
#ifdef MBS_TEST_TETGEN
    MBS_CHECK(result.part_a.volume_metrics.has_value());
    MBS_CHECK(result.part_b.volume_metrics.has_value());
    for (const auto* metrics : {&*result.part_a.volume_metrics, &*result.part_b.volume_metrics}) {
        MBS_CHECK(metrics->tetrahedra > 0);
        MBS_CHECK(metrics->minimum_signed_volume > 1.0e-9);
        MBS_CHECK(metrics->non_positive_count == 0);
        MBS_CHECK(metrics->small_volume_count == 0);
    }

    const auto test_directory = std::filesystem::temp_directory_path() /
                                "mbs-stage6-contact-risk-contract";
    std::error_code cleanup_error;
    std::filesystem::remove_all(test_directory, cleanup_error);
    const auto published = mbs::geometry::publish_geometry(result, config, test_directory);
    const auto report = mbs::geometry::analyze_contact_risk(published.directory);
    MBS_CHECK(report.part_a.element_count > 0);
    MBS_CHECK(report.part_b.element_count > 0);
    MBS_CHECK(report.part_a.minimum_volume > 0.0);
    MBS_CHECK(report.part_b.minimum_volume > 0.0);
    MBS_CHECK(std::isfinite(report.contact.maximum_penetration));
    MBS_CHECK(report.risk_level == "LOW" || report.risk_level == "MEDIUM" ||
              report.risk_level == "HIGH");
    MBS_CHECK(report.contact.maximum_penetration >= 0.0);
    MBS_CHECK(report.contact.mean_positive_penetration >= 0.0);
    MBS_CHECK(report.contact.mean_positive_penetration <=
              report.contact.maximum_penetration);
    MBS_CHECK(report.adjustment.affected_element_count <=
              report.part_a.element_count + report.part_b.element_count);
    MBS_CHECK(report.adjustment.inverted_element_count == 0);
    MBS_CHECK(report.adjustment.minimum_post_adjustment_volume_ratio > 0.0);
    mbs::geometry::save_contact_risk(report,
                                     published.directory / "contact_risk_report.json");
    MBS_CHECK(std::filesystem::is_regular_file(published.directory /
                                               "contact_risk_report.json"));
    std::filesystem::remove_all(test_directory, cleanup_error);

    auto quadratic_config = config;
    quadratic_config.mesh.tetgen.order = 2;
    quadratic_config.mesh.tetgen.target_edge_length_mm = 1.5;
    quadratic_config.mesh.tetgen.optimization_level = 2;
    const auto quadratic = GeometryKernel{}.generate(quadratic_config);
    for (const auto* volume : {&*quadratic.part_a.volume, &*quadratic.part_b.volume}) {
        MBS_CHECK(volume->order == 2);
        MBS_CHECK(volume->nodes_per_element() == 10);
        const std::array<std::array<std::size_t, 2>, 6> edges{
            {{{0, 1}}, {{1, 2}}, {{2, 0}}, {{0, 3}}, {{1, 3}}, {{2, 3}}}};
        for (const auto& tetrahedron : volume->tetrahedra) {
            for (std::size_t edge = 0; edge < edges.size(); ++edge) {
                const auto& first = volume->points[tetrahedron[edges[edge][0]]];
                const auto& second = volume->points[tetrahedron[edges[edge][1]]];
                const auto& middle = volume->points[tetrahedron[4 + edge]];
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    MBS_CHECK(close(middle[axis], (first[axis] + second[axis]) * 0.5,
                                    1.0e-9, 1.0e-9));
                }
            }
        }
    }
    const auto quadratic_directory = std::filesystem::temp_directory_path() /
                                     "mbs-stage7-tet10-contract";
    std::filesystem::remove_all(quadratic_directory, cleanup_error);
    const auto quadratic_published =
        mbs::geometry::publish_geometry(quadratic, quadratic_config, quadratic_directory);
    {
        std::ifstream inp{quadratic_published.directory / "tpms-tet-A.inp"};
        const std::string content{std::istreambuf_iterator<char>{inp}, {}};
        MBS_CHECK(content.find("*Element, type=C3D10") != std::string::npos);
    }
    {
        std::ifstream vtk{quadratic_published.directory / "visualization" /
                          "tetrahedralization" / "part-A.vtu"};
        const std::string content{std::istreambuf_iterator<char>{vtk}, {}};
        MBS_CHECK(content.find("UnstructuredGrid") != std::string::npos);
        MBS_CHECK(content.find("Name=\"types\"") != std::string::npos);
        MBS_CHECK(content.find("24 ") != std::string::npos);
    }
    const auto quadratic_risk =
        mbs::geometry::analyze_contact_risk(quadratic_published.directory);
    MBS_CHECK(quadratic_risk.part_a.element_count > 0);
    MBS_CHECK(quadratic_risk.part_b.element_count > 0);
    MBS_CHECK(quadratic_risk.part_a.minimum_volume > 0.0);
    MBS_CHECK(quadratic_risk.part_b.minimum_volume > 0.0);
    std::filesystem::remove_all(quadratic_directory, cleanup_error);
#else
    MBS_CHECK(!result.part_a.volume_metrics.has_value());
    MBS_CHECK(!result.part_b.volume_metrics.has_value());
#endif

    return failures == 0 ? 0 : 1;
}
