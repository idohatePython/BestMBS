#include "mbs/geometry/MeshDoctor.hpp"

#ifdef MBS_ENABLE_CGAL
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Polygon_mesh_processing/autorefinement.h>
#include <CGAL/Polygon_mesh_processing/Adaptive_sizing_field.h>
#include <CGAL/Polygon_mesh_processing/angle_and_area_smoothing.h>
#include <CGAL/Polygon_mesh_processing/detect_features.h>
#include <CGAL/Polygon_mesh_processing/distance.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/repair_degeneracies.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Surface_mesh_simplification/edge_collapse.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Bounded_normal_change_filter.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Edge_count_ratio_stop_predicate.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/LindstromTurk_cost.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/LindstromTurk_placement.h>
#include <CGAL/Surface_mesh_simplification/Policies/Edge_collapse/Polyhedral_envelope_filter.h>
#include <CGAL/number_utils.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <numeric>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace mbs::geometry {
namespace {

using Edge = std::array<std::uint32_t, 2>;
using FaceKey = std::array<std::uint32_t, 3>;

Edge edge_key(std::uint32_t first, std::uint32_t second) {
    if (second < first) {
        std::swap(first, second);
    }
    return {first, second};
}

class DisjointSet final {
  public:
    explicit DisjointSet(const std::size_t size) : parent_(size), rank_(size) {
        std::iota(parent_.begin(), parent_.end(), std::size_t{});
    }

    std::size_t find(const std::size_t value) {
        if (parent_[value] != value) {
            parent_[value] = find(parent_[value]);
        }
        return parent_[value];
    }

    void merge(const std::size_t first, const std::size_t second) {
        auto a = find(first);
        auto b = find(second);
        if (a == b) {
            return;
        }
        if (rank_[a] < rank_[b]) {
            std::swap(a, b);
        }
        parent_[b] = a;
        if (rank_[a] == rank_[b]) {
            ++rank_[a];
        }
    }

  private:
    std::vector<std::size_t> parent_;
    std::vector<unsigned char> rank_;
};

double doubled_triangle_area(const SurfaceMesh& mesh,
                             const std::array<std::uint32_t, 3>& triangle) {
    const auto& a = mesh.points[triangle[0]];
    const auto& b = mesh.points[triangle[1]];
    const auto& c = mesh.points[triangle[2]];
    const std::array ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array cross{ab[1] * ac[2] - ab[2] * ac[1],
                           ab[2] * ac[0] - ab[0] * ac[2],
                           ab[0] * ac[1] - ab[1] * ac[0]};
    return std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] +
                     cross[2] * cross[2]);
}

double enclosed_volume(const SurfaceMesh& mesh) {
    double six_times_volume = 0.0;
    for (const auto& triangle : mesh.triangles) {
        const auto& a = mesh.points[triangle[0]];
        const auto& b = mesh.points[triangle[1]];
        const auto& c = mesh.points[triangle[2]];
        const std::array cross{b[1] * c[2] - b[2] * c[1],
                               b[2] * c[0] - b[0] * c[2],
                               b[0] * c[1] - b[1] * c[0]};
        six_times_volume += a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2];
    }
    return std::abs(six_times_volume) / 6.0;
}

std::string metric_line(const std::string_view label, const std::size_t value,
                        const std::string_view expectation, const std::string_view explanation) {
    std::ostringstream stream;
    stream << "  - " << label << ": " << value << "（期望" << expectation << "）— "
           << explanation;
    return stream.str();
}

SurfaceMesh clean_triangle_soup(const SurfaceMesh& input) {
    SurfaceMesh output;
    std::map<std::array<double, 3>, std::uint32_t> canonical_points;
    std::vector<std::uint32_t> remap(input.points.size());
    for (std::size_t index = 0; index < input.points.size(); ++index) {
        const auto [position, inserted] = canonical_points.try_emplace(
            input.points[index], static_cast<std::uint32_t>(output.points.size()));
        remap[index] = position->second;
        if (inserted) {
            output.points.push_back(input.points[index]);
        }
    }
    std::set<FaceKey> unique_faces;
    for (const auto& source : input.triangles) {
        if (std::ranges::any_of(source, [&remap](const auto vertex) {
                return static_cast<std::size_t>(vertex) >= remap.size();
            })) {
            continue;
        }
        const std::array<std::uint32_t, 3> triangle{
            remap[source[0]], remap[source[1]], remap[source[2]]};
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
            triangle[2] == triangle[0]) {
            continue;
        }
        auto key = triangle;
        std::ranges::sort(key);
        if (unique_faces.insert(key).second && doubled_triangle_area(output, triangle) > 1.0e-14) {
            output.triangles.push_back(triangle);
        }
    }
    std::vector<bool> used(output.points.size());
    for (const auto& triangle : output.triangles) {
        for (const auto vertex : triangle) {
            used[vertex] = true;
        }
    }
    SurfaceMesh compact;
    std::vector<std::uint32_t> compact_index(output.points.size());
    for (std::size_t index = 0; index < output.points.size(); ++index) {
        if (used[index]) {
            compact_index[index] = static_cast<std::uint32_t>(compact.points.size());
            compact.points.push_back(output.points[index]);
        }
    }
    compact.triangles.reserve(output.triangles.size());
    for (const auto& triangle : output.triangles) {
        compact.triangles.push_back({compact_index[triangle[0]], compact_index[triangle[1]],
                                     compact_index[triangle[2]]});
    }
    return compact;
}

#ifdef MBS_ENABLE_CGAL
using ExactKernel = CGAL::Exact_predicates_exact_constructions_kernel;
using ExactPoint = ExactKernel::Point_3;
using ExactTriangle = std::array<std::size_t, 3>;

std::vector<std::pair<std::size_t, std::size_t>> self_intersections(const SurfaceMesh& mesh) {
    std::vector<ExactPoint> points;
    points.reserve(mesh.points.size());
    for (const auto& point : mesh.points) {
        points.emplace_back(point[0], point[1], point[2]);
    }
    std::vector<ExactTriangle> triangles;
    triangles.reserve(mesh.triangles.size());
    for (const auto& triangle : mesh.triangles) {
        triangles.push_back({triangle[0], triangle[1], triangle[2]});
    }
    std::vector<std::pair<std::size_t, std::size_t>> intersections;
    CGAL::Polygon_mesh_processing::triangle_soup_self_intersections(
        points, triangles, std::back_inserter(intersections));
    return intersections;
}

SurfaceMesh autorefine(const SurfaceMesh& input) {
    std::vector<ExactPoint> points;
    points.reserve(input.points.size());
    for (const auto& point : input.points) {
        points.emplace_back(point[0], point[1], point[2]);
    }
    std::vector<ExactTriangle> triangles;
    triangles.reserve(input.triangles.size());
    for (const auto& triangle : input.triangles) {
        triangles.push_back({triangle[0], triangle[1], triangle[2]});
    }
    CGAL::Polygon_mesh_processing::autorefine_triangle_soup(points, triangles);

    SurfaceMesh output;
    output.points.reserve(points.size());
    for (const auto& point : points) {
        output.points.push_back(
            {CGAL::to_double(point.x()), CGAL::to_double(point.y()), CGAL::to_double(point.z())});
    }
    output.triangles.reserve(triangles.size());
    for (const auto& triangle : triangles) {
        if (triangle[0] > std::numeric_limits<std::uint32_t>::max() ||
            triangle[1] > std::numeric_limits<std::uint32_t>::max() ||
            triangle[2] > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error{"CGAL repair exceeded the 32-bit mesh index limit"};
        }
        output.triangles.push_back({static_cast<std::uint32_t>(triangle[0]),
                                    static_cast<std::uint32_t>(triangle[1]),
                                    static_cast<std::uint32_t>(triangle[2])});
    }
    return output;
}

SurfaceMesh remove_self_intersections_preserving_surface(const SurfaceMesh& input,
                                                         bool& repaired) {
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Mesh = CGAL::Surface_mesh<Kernel::Point_3>;
    Mesh mesh;
    std::vector<Mesh::Vertex_index> vertices;
    vertices.reserve(input.points.size());
    for (const auto& point : input.points) {
        vertices.push_back(mesh.add_vertex({point[0], point[1], point[2]}));
    }
    for (const auto& triangle : input.triangles) {
        const auto face = mesh.add_face(vertices[triangle[0]], vertices[triangle[1]],
                                        vertices[triangle[2]]);
        if (face == Mesh::null_face()) {
            throw std::runtime_error{"CGAL could not reconstruct the input manifold surface"};
        }
    }
    repaired = CGAL::Polygon_mesh_processing::experimental::remove_self_intersections(
        mesh, CGAL::parameters::number_of_iterations(5)
                  .preserve_genus(true)
                  .use_smoothing(false));

    SurfaceMesh output;
    std::map<Mesh::Vertex_index, std::uint32_t> vertex_ids;
    output.points.reserve(mesh.number_of_vertices());
    for (const auto vertex : mesh.vertices()) {
        const auto& point = mesh.point(vertex);
        vertex_ids.emplace(vertex, static_cast<std::uint32_t>(output.points.size()));
        output.points.push_back({point.x(), point.y(), point.z()});
    }
    output.triangles.reserve(mesh.number_of_faces());
    for (const auto face : mesh.faces()) {
        std::array<std::uint32_t, 3> triangle{};
        std::size_t index = 0;
        for (const auto vertex : CGAL::vertices_around_face(mesh.halfedge(face), mesh)) {
            if (index >= triangle.size()) {
                throw std::runtime_error{"CGAL self-intersection repair produced a non-triangle"};
            }
            triangle[index++] = vertex_ids.at(vertex);
        }
        if (index != triangle.size()) {
            throw std::runtime_error{"CGAL self-intersection repair produced an invalid face"};
        }
        output.triangles.push_back(triangle);
    }
    return output;
}

using InexactKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CgalMesh = CGAL::Surface_mesh<InexactKernel::Point_3>;

CgalMesh to_cgal_mesh(const SurfaceMesh& input) {
    CgalMesh mesh;
    std::vector<CgalMesh::Vertex_index> vertices;
    vertices.reserve(input.points.size());
    for (const auto& point : input.points) {
        vertices.push_back(mesh.add_vertex({point[0], point[1], point[2]}));
    }
    for (const auto& triangle : input.triangles) {
        if (triangle[0] >= vertices.size() || triangle[1] >= vertices.size() ||
            triangle[2] >= vertices.size() ||
            mesh.add_face(vertices[triangle[0]], vertices[triangle[1]],
                          vertices[triangle[2]]) == CgalMesh::null_face()) {
            throw std::runtime_error{"CGAL could not reconstruct a manifold triangle surface"};
        }
    }
    return mesh;
}

SurfaceMesh from_cgal_mesh(const CgalMesh& mesh) {
    SurfaceMesh output;
    std::map<CgalMesh::Vertex_index, std::uint32_t> ids;
    output.points.reserve(mesh.number_of_vertices());
    for (const auto vertex : mesh.vertices()) {
        const auto& point = mesh.point(vertex);
        ids.emplace(vertex, static_cast<std::uint32_t>(output.points.size()));
        output.points.push_back({point.x(), point.y(), point.z()});
    }
    output.triangles.reserve(mesh.number_of_faces());
    for (const auto face : mesh.faces()) {
        std::array<std::uint32_t, 3> triangle{};
        std::size_t index{};
        for (const auto vertex : CGAL::vertices_around_face(mesh.halfedge(face), mesh)) {
            if (index >= triangle.size()) {
                throw std::runtime_error{"CGAL remeshing produced a non-triangle face"};
            }
            triangle[index++] = ids.at(vertex);
        }
        if (index != triangle.size()) {
            throw std::runtime_error{"CGAL remeshing produced an incomplete triangle face"};
        }
        output.triangles.push_back(triangle);
    }
    return output;
}
#endif

} // namespace

bool SurfaceDefects::acceptable_for_volume_meshing() const noexcept {
    return boundary_edges == 0 && connected_components == 1 && non_manifold_edges == 0 &&
           non_manifold_vertices == 0 && holes == 0 && unreferenced_vertices == 0 &&
           degenerate_faces == 0 && duplicate_faces == 0 && self_intersection_pairs == 0 &&
           two_manifold;
}

std::vector<std::string> SurfaceDefects::readable_lines(const std::string_view part) const {
    std::vector<std::string> lines;
    lines.emplace_back("[曲面质量][" + std::string{part} + "] " +
                       (acceptable_for_volume_meshing() ? "通过" : "需要修复"));
    lines.push_back(metric_line("边界边", boundary_edges, "为 0", "非零表示曲面没有完全闭合"));
    lines.push_back(
        metric_line("连通域", connected_components, "为 1", "多个独立碎片会破坏实体定义"));
    lines.push_back(metric_line("非二流形边", non_manifold_edges, "为 0",
                                "一条边不能同时属于两个以上的表面片"));
    lines.push_back(metric_line("非二流形点", non_manifold_vertices, "为 0",
                                "顶点邻域应当是一张连续圆盘"));
    lines.push_back(metric_line("孔洞", holes, "为 0", "孔洞会使内外区域无法可靠判定"));
    lines.push_back(metric_line("未引用顶点", unreferenced_vertices, "为 0",
                                "这些点不属于任何三角面，可安全清理"));
    lines.push_back(metric_line("退化三角形", degenerate_faces, "为 0",
                                "零面积或重复顶点三角形会导致数值不稳定"));
    lines.push_back(metric_line("重复三角形", duplicate_faces, "为 0",
                                "重叠面会造成内部区域歧义"));
    lines.push_back(metric_line("自相交面对", self_intersection_pairs, "为 0",
                                "相交三角形是 TetGen 错误码 3 的直接原因"));
    return lines;
}

SurfaceDefects inspect_surface(const SurfaceMesh& mesh) {
    SurfaceDefects result;
    if (mesh.triangles.empty()) {
        return result;
    }

    std::map<Edge, std::vector<std::size_t>> edge_faces;
    std::set<FaceKey> unique_faces;
    std::vector<bool> referenced(mesh.points.size());
    DisjointSet face_components(mesh.triangles.size());
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        const auto& triangle = mesh.triangles[face];
        if (std::ranges::any_of(triangle, [&mesh](const auto vertex) {
                return static_cast<std::size_t>(vertex) >= mesh.points.size();
            })) {
            ++result.degenerate_faces;
            continue;
        }
        for (const auto vertex : triangle) {
            referenced[vertex] = true;
        }
        auto key = triangle;
        std::ranges::sort(key);
        result.duplicate_faces += !unique_faces.insert(key).second ? 1U : 0U;
        result.degenerate_faces +=
            triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
                    triangle[2] == triangle[0] || doubled_triangle_area(mesh, triangle) <= 1.0e-14
                ? 1U
                : 0U;
        edge_faces[edge_key(triangle[0], triangle[1])].push_back(face);
        edge_faces[edge_key(triangle[1], triangle[2])].push_back(face);
        edge_faces[edge_key(triangle[2], triangle[0])].push_back(face);
    }
    result.unreferenced_vertices =
        static_cast<std::size_t>(std::ranges::count(referenced, false));

    std::map<std::uint32_t, std::vector<std::uint32_t>> boundary_graph;
    std::set<std::uint32_t> non_manifold_vertex_set;
    for (const auto& [edge, faces] : edge_faces) {
        if (faces.size() == 1) {
            ++result.boundary_edges;
            boundary_graph[edge[0]].push_back(edge[1]);
            boundary_graph[edge[1]].push_back(edge[0]);
        } else if (faces.size() > 2) {
            ++result.non_manifold_edges;
            result.incident_faces_on_non_manifold_edges += faces.size();
            non_manifold_vertex_set.insert(edge[0]);
            non_manifold_vertex_set.insert(edge[1]);
        }
        for (std::size_t index = 1; index < faces.size(); ++index) {
            face_components.merge(faces.front(), faces[index]);
        }
    }
    std::set<std::size_t> component_roots;
    for (std::size_t face = 0; face < mesh.triangles.size(); ++face) {
        component_roots.insert(face_components.find(face));
    }
    result.connected_components = component_roots.size();

    std::set<std::uint32_t> visited_boundary;
    for (const auto& [start, _] : boundary_graph) {
        if (visited_boundary.contains(start)) {
            continue;
        }
        ++result.holes;
        std::vector<std::uint32_t> stack{start};
        while (!stack.empty()) {
            const auto vertex = stack.back();
            stack.pop_back();
            if (!visited_boundary.insert(vertex).second) {
                continue;
            }
            for (const auto neighbour : boundary_graph[vertex]) {
                stack.push_back(neighbour);
            }
        }
    }
    for (const auto& [vertex, neighbours] : boundary_graph) {
        if (neighbours.size() != 2) {
            non_manifold_vertex_set.insert(vertex);
        }
    }
    result.non_manifold_vertices = non_manifold_vertex_set.size();
    result.incident_faces_on_non_manifold_vertices = result.non_manifold_vertices;

#ifdef MBS_ENABLE_CGAL
    const auto intersections = self_intersections(mesh);
    result.self_intersection_pairs = intersections.size();
    std::set<std::size_t> intersecting_faces;
    for (const auto& [first, second] : intersections) {
        intersecting_faces.insert(first);
        intersecting_faces.insert(second);
    }
    result.self_intersecting_faces = intersecting_faces.size();
#endif
    result.two_manifold = result.boundary_edges == 0 && result.non_manifold_edges == 0 &&
                          result.non_manifold_vertices == 0;
    return result;
}

MeshRepairResult repair_surface(const SurfaceMesh& mesh, const std::size_t maximum_rounds) {
    MeshRepairResult result{.mesh = mesh,
                            .before = inspect_surface(mesh),
                            .after = {},
                            .changed = false,
                            .accepted = false,
                            .backend = "cgal-pmp-6.0.3",
                            .detail = {},
                            .volume_relative_change = 0.0};
    result.after = result.before;
    if (result.before.acceptable_for_volume_meshing()) {
        result.accepted = true;
        result.detail = "surface already satisfies all volume-meshing gates";
        return result;
    }
#ifdef MBS_ENABLE_CGAL
    bool polygon_mesh_repaired = false;
    try {
        auto candidate = remove_self_intersections_preserving_surface(result.mesh,
                                                                      polygon_mesh_repaired);
        auto candidate_defects = inspect_surface(candidate);
        const auto original_volume = enclosed_volume(mesh);
        const auto candidate_volume = enclosed_volume(candidate);
        const auto denominator = std::max(original_volume, 1.0e-12);
        const auto volume_change = std::abs(candidate_volume - original_volume) / denominator;
        if (polygon_mesh_repaired && candidate_defects.acceptable_for_volume_meshing() &&
            volume_change <= 0.01) {
            result.mesh = std::move(candidate);
            result.after = candidate_defects;
            result.changed = true;
            result.accepted = true;
            result.volume_relative_change = volume_change;
            result.detail = "CGAL removed self-intersections while preserving the closed surface";
            return result;
        }
    } catch (const std::exception&) {
        // The exact triangle-soup path below is deliberately independent and remains available.
    }
    for (std::size_t round = 0; round < maximum_rounds; ++round) {
        if (result.after.self_intersection_pairs == 0) {
            break;
        }
        result.mesh = clean_triangle_soup(autorefine(result.mesh));
        result.changed = true;
        result.after = inspect_surface(result.mesh);
        if (result.after.acceptable_for_volume_meshing()) {
            break;
        }
    }
    result.accepted = result.after.acceptable_for_volume_meshing();
    if (result.accepted) {
        const auto original_volume = enclosed_volume(mesh);
        result.volume_relative_change =
            std::abs(enclosed_volume(result.mesh) - original_volume) /
            std::max(original_volume, 1.0e-12);
        result.accepted = result.volume_relative_change <= 0.01;
    }
    result.detail = result.accepted ? "CGAL removed all detected surface defects"
                                    : "CGAL repair stopped with remaining quality defects";
#else
    result.detail = "CGAL support is disabled in this build";
#endif
    return result;
}

RemeshResult remesh_surface(const SurfaceMesh& input, const SurfaceRemeshOptions& options) {
    if (!std::isfinite(options.target_edge_length) || options.target_edge_length <= 0.0 ||
        !std::isfinite(options.surface_tolerance) || options.surface_tolerance <= 0.0 ||
        !std::isfinite(options.minimum_edge_length) || options.minimum_edge_length <= 0.0 ||
        !std::isfinite(options.maximum_edge_length) ||
        options.maximum_edge_length < options.minimum_edge_length || options.iterations < 1 ||
        !std::isfinite(options.feature_angle_degrees) ||
        options.feature_angle_degrees <= 0.0 || options.feature_angle_degrees > 180.0 ||
        !std::isfinite(options.simplify_keep_ratio) || options.simplify_keep_ratio < 0.1 ||
        options.simplify_keep_ratio > 1.0) {
        throw std::invalid_argument{"invalid CGAL surface remeshing settings"};
    }
    RemeshResult result{.mesh = input,
                        .changed = false,
                        .sharp_edges = 0,
                        .target_edge_length = options.target_edge_length,
                        .backend = options.sizing_mode == domain::SurfaceSizingMode::uniform
                                       ? "cgal-pmp-uniform-remeshing-6.0.3"
                                       : "cgal-pmp-adaptive-remeshing-6.0.3",
                        .detail = {}};
#ifdef MBS_ENABLE_CGAL
    auto mesh = to_cgal_mesh(input);
    if (!CGAL::is_triangle_mesh(mesh) || !CGAL::is_closed(mesh)) {
        throw std::runtime_error{"CGAL isotropic remeshing requires a closed triangle mesh"};
    }
    // Marching tetrahedra can emit geometrically zero-area faces while retaining a
    // valid combinatorial manifold.  PMP repairs these by local collapses before the
    // split/collapse/flip remeshing loop; deleting soup faces would open the solid.
    CGAL::Polygon_mesh_processing::remove_degenerate_faces(mesh);
    if (!CGAL::is_triangle_mesh(mesh) || !CGAL::is_closed(mesh)) {
        throw std::runtime_error{"CGAL degenerate-face repair opened the input surface"};
    }
    const auto input_reference = mesh;
    auto feature_map = mesh.add_property_map<CgalMesh::Edge_index, bool>("e:sharp", false).first;
    std::vector<CgalMesh::Edge_index> sharp_edges;
    if (options.sharpen) {
        CGAL::Polygon_mesh_processing::detect_sharp_edges(
            mesh, options.feature_angle_degrees, feature_map);
        for (const auto edge : mesh.edges()) {
            if (get(feature_map, edge)) {
                sharp_edges.push_back(edge);
            }
        }
    }
    result.sharp_edges = sharp_edges.size();
    // Protected feature polylines must first be sampled at the requested length;
    // otherwise long plate corners force fan-shaped triangles around them.
    const auto feature_edge_length =
        options.sizing_mode == domain::SurfaceSizingMode::uniform
            ? options.target_edge_length
            : options.maximum_edge_length;
    CGAL::Polygon_mesh_processing::split_long_edges(
        sharp_edges, feature_edge_length, mesh,
        CGAL::parameters::edge_is_constrained_map(feature_map));
    const auto remesh_parameters =
        CGAL::parameters::number_of_iterations(options.iterations)
            .number_of_relaxation_steps(3)
            .edge_is_constrained_map(feature_map)
            .protect_constraints(options.sharpen)
            .do_project(true);
    if (options.sizing_mode == domain::SurfaceSizingMode::curvature_adaptive) {
        CGAL::Polygon_mesh_processing::Adaptive_sizing_field<CgalMesh> sizing_field(
            options.surface_tolerance,
            std::pair{options.minimum_edge_length, options.maximum_edge_length}, faces(mesh),
            mesh);
        CGAL::Polygon_mesh_processing::isotropic_remeshing(faces(mesh), sizing_field, mesh,
                                                           remesh_parameters);
    } else {
        CGAL::Polygon_mesh_processing::isotropic_remeshing(
            faces(mesh), options.target_edge_length, mesh, remesh_parameters);
    }
    if (options.sharpen) {
        feature_map =
            mesh.add_property_map<CgalMesh::Edge_index, bool>("e:sharp-final", false).first;
        for (const auto edge : mesh.edges()) {
            put(feature_map, edge, false);
        }
        CGAL::Polygon_mesh_processing::detect_sharp_edges(
            mesh, options.feature_angle_degrees, feature_map);
        CGAL::Polygon_mesh_processing::angle_and_area_smoothing(
            faces(mesh), mesh,
            CGAL::parameters::number_of_iterations(1)
                .use_angle_smoothing(true)
                .use_area_smoothing(false)
                .edge_is_constrained_map(feature_map)
                .use_safety_constraints(true)
                .do_project(true));
    }
    const auto remeshed_reference = mesh;
    if (options.simplify) {
        namespace SMS = CGAL::Surface_mesh_simplification;
        using Cost = SMS::LindstromTurk_cost<CgalMesh>;
        using Placement = SMS::LindstromTurk_placement<CgalMesh>;
        using Filter = SMS::Polyhedral_envelope_filter<
            InexactKernel, SMS::Bounded_normal_change_filter<>>;
        SMS::Edge_count_ratio_stop_predicate<CgalMesh> stop(options.simplify_keep_ratio);
        Filter filter(options.surface_tolerance);
        SMS::edge_collapse(
            mesh, stop,
            CGAL::parameters::get_cost(Cost())
                .get_placement(Placement())
                .filter(filter)
                .edge_is_constrained_map(feature_map));
        auto simplified = from_cgal_mesh(mesh);
        const auto simplified_defects = inspect_surface(simplified);
        const auto deviation = CGAL::Polygon_mesh_processing::bounded_error_symmetric_Hausdorff_distance<
            CGAL::Sequential_tag>(remeshed_reference, mesh,
                                  std::max(options.surface_tolerance * 0.1, 1.0e-8));
        if (!simplified_defects.acceptable_for_volume_meshing() ||
            deviation > options.surface_tolerance) {
            mesh = remeshed_reference;
            result.simplification_rolled_back = true;
        } else {
            result.simplified = true;
            result.surface_deviation = deviation;
        }
    }
    result.mesh = from_cgal_mesh(mesh);
    result.changed = true;
    result.surface_deviation =
        CGAL::Polygon_mesh_processing::bounded_error_symmetric_Hausdorff_distance<
            CGAL::Sequential_tag>(input_reference, mesh,
                                  std::max(options.surface_tolerance * 0.1, 1.0e-8));

    std::vector<double> edge_lengths;
    edge_lengths.reserve(result.mesh.triangles.size() * 3);
    result.minimum_angle_degrees = 180.0;
    double aspect_sum{};
    for (const auto& triangle : result.mesh.triangles) {
        std::array<double, 3> lengths{};
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto& a = result.mesh.points[triangle[edge]];
            const auto& b = result.mesh.points[triangle[(edge + 1) % 3]];
            lengths[edge] = std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
            edge_lengths.push_back(lengths[edge]);
        }
        const auto [shortest, longest] = std::minmax_element(lengths.begin(), lengths.end());
        const auto aspect = *longest / std::max(*shortest, 1.0e-15);
        aspect_sum += aspect;
        result.maximum_aspect_ratio = std::max(result.maximum_aspect_ratio, aspect);
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            const auto adjacent_a = lengths[vertex];
            const auto adjacent_b = lengths[(vertex + 2) % 3];
            const auto opposite = lengths[(vertex + 1) % 3];
            const auto cosine = std::clamp(
                (adjacent_a * adjacent_a + adjacent_b * adjacent_b - opposite * opposite) /
                    std::max(2.0 * adjacent_a * adjacent_b, 1.0e-30),
                -1.0, 1.0);
            result.minimum_angle_degrees =
                std::min(result.minimum_angle_degrees,
                         std::acos(cosine) * 180.0 / std::numbers::pi);
        }
    }
    result.mean_aspect_ratio = result.mesh.triangles.empty()
                                   ? 0.0
                                   : aspect_sum /
                                         static_cast<double>(result.mesh.triangles.size());
    if (!edge_lengths.empty()) {
        const auto mean = std::accumulate(edge_lengths.begin(), edge_lengths.end(), 0.0) /
                          static_cast<double>(edge_lengths.size());
        const auto [minimum, maximum] =
            std::minmax_element(edge_lengths.begin(), edge_lengths.end());
        result.minimum_edge_length = *minimum;
        result.mean_edge_length = mean;
        result.maximum_edge_length = *maximum;
        double variance{};
        for (const auto length : edge_lengths) {
            variance += (length - mean) * (length - mean);
        }
        result.edge_length_cv =
            std::sqrt(variance / static_cast<double>(edge_lengths.size())) /
                                std::max(mean, 1.0e-15);
    }
    std::ostringstream detail;
    detail << "mode=" << domain::to_string(options.sizing_mode)
           << ", target=" << options.target_edge_length
           << " mm, tolerance=" << options.surface_tolerance
           << " mm, iterations=" << options.iterations
           << ", protected sharp edges=" << result.sharp_edges
           << ", simplified=" << (result.simplified ? "yes" : "no")
           << ", simplify rollback="
           << (result.simplification_rolled_back ? "yes" : "no");
    result.detail = detail.str();
#else
    result.backend = "disabled";
    result.detail = "CGAL support is disabled in this build";
    throw std::runtime_error{result.detail};
#endif
    return result;
}

RemeshResult isotropic_remesh(const SurfaceMesh& input, const double target_edge_length,
                              const int iterations, const double feature_angle_degrees,
                              const bool preserve_sharp_features) {
    return remesh_surface(input,
                          {.sizing_mode = domain::SurfaceSizingMode::uniform,
                           .target_edge_length = target_edge_length,
                           .surface_tolerance = std::max(target_edge_length * 0.25, 1.0e-6),
                           .minimum_edge_length = target_edge_length,
                           .maximum_edge_length = target_edge_length,
                           .iterations = iterations,
                           .feature_angle_degrees = feature_angle_degrees,
                           .sharpen = preserve_sharp_features,
                           .simplify = false,
                           .simplify_keep_ratio = 0.8});
}

} // namespace mbs::geometry
