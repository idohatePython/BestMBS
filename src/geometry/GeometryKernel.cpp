#include "mbs/geometry/GeometryKernel.hpp"
#include "mbs/geometry/MeshDoctor.hpp"

#include <manifold/manifold.h>

#ifdef MBS_ENABLE_TETGEN
#include <tetgen.h>
#endif

#ifdef MBS_ENABLE_VTK_IO
#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkUnstructuredGrid.h>
#include <vtkXMLImageDataWriter.h>
#include <vtkXMLPolyDataWriter.h>
#include <vtkXMLUnstructuredGridWriter.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mbs::geometry {
namespace {

using manifold::Box;
using manifold::Manifold;
using manifold::MeshGL64;
using manifold::vec3;

constexpr double small_tetrahedron_volume = 1.0e-9;

void report(const GeometryCallbacks& callbacks, const double progress,
            const std::string_view message) {
    if (callbacks.cancelled && callbacks.cancelled()) {
        throw std::runtime_error{"geometry generation cancelled"};
    }
    if (callbacks.progress) {
        callbacks.progress(progress, message);
    }
}

class DisjointSet final {
  public:
    explicit DisjointSet(const std::size_t size) : parent_(size) {
        for (std::size_t index = 0; index < size; ++index) {
            parent_[index] = index;
        }
    }

    std::size_t find(const std::size_t value) {
        auto root = value;
        while (parent_[root] != root) {
            root = parent_[root];
        }
        auto current = value;
        while (parent_[current] != current) {
            const auto next = parent_[current];
            parent_[current] = root;
            current = next;
        }
        return root;
    }

    void merge(const std::size_t first, const std::size_t second) {
        const auto first_root = find(first);
        const auto second_root = find(second);
        if (first_root != second_root) {
            parent_[first_root] = second_root;
        }
    }

  private:
    std::vector<std::size_t> parent_;
};

SurfaceMesh surface_mesh(const Manifold& solid) {
    const MeshGL64 mesh = solid.GetMeshGL64();
    if (mesh.numProp < 3 || mesh.vertProperties.size() % mesh.numProp != 0 ||
        mesh.triVerts.size() % 3 != 0) {
        throw std::runtime_error{"Manifold returned an invalid MeshGL64 layout"};
    }

    if (mesh.mergeFromVert.size() != mesh.mergeToVert.size()) {
        throw std::runtime_error{"Manifold returned mismatched topology merge vectors"};
    }
    DisjointSet topology(mesh.NumVert());
    for (std::size_t index = 0; index < mesh.mergeFromVert.size(); ++index) {
        const auto from = static_cast<std::size_t>(mesh.mergeFromVert[index]);
        const auto to = static_cast<std::size_t>(mesh.mergeToVert[index]);
        if (from >= mesh.NumVert() || to >= mesh.NumVert()) {
            throw std::runtime_error{"Manifold returned an out-of-range topology merge"};
        }
        topology.merge(from, to);
    }

    SurfaceMesh result;
    result.points.reserve(mesh.NumVert());
    std::vector<std::uint32_t> canonical(mesh.NumVert());
    std::map<std::size_t, std::uint32_t> root_to_vertex;
    for (std::size_t index = 0; index < mesh.NumVert(); ++index) {
        const auto root = topology.find(index);
        const auto existing = root_to_vertex.find(root);
        if (existing != root_to_vertex.end()) {
            canonical[index] = existing->second;
            continue;
        }
        const auto offset = index * mesh.numProp;
        const std::array point{mesh.vertProperties[offset], mesh.vertProperties[offset + 1],
                               mesh.vertProperties[offset + 2]};
        if (!std::ranges::all_of(point, [](const double value) { return std::isfinite(value); })) {
            throw std::runtime_error{"Manifold returned a non-finite surface vertex"};
        }
        if (result.points.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error{"surface mesh exceeds the 32-bit index limit"};
        }
        const auto output_index = static_cast<std::uint32_t>(result.points.size());
        root_to_vertex.emplace(root, output_index);
        canonical[index] = output_index;
        result.points.push_back(point);
    }
    result.triangles.reserve(mesh.NumTri());
    for (std::size_t index = 0; index < mesh.triVerts.size(); index += 3) {
        const std::array source_triangle{mesh.triVerts[index], mesh.triVerts[index + 1],
                                         mesh.triVerts[index + 2]};
        if (std::ranges::any_of(source_triangle, [&mesh](const auto vertex) {
                return static_cast<std::size_t>(vertex) >= mesh.NumVert();
            })) {
            throw std::runtime_error{"Manifold returned an out-of-range triangle index"};
        }
        const std::array<std::uint32_t, 3> triangle{
            canonical[static_cast<std::size_t>(source_triangle[0])],
            canonical[static_cast<std::size_t>(source_triangle[1])],
            canonical[static_cast<std::size_t>(source_triangle[2])]};
        if (triangle[0] != triangle[1] && triangle[1] != triangle[2] &&
            triangle[2] != triangle[0]) {
            result.triangles.push_back(triangle);
        }
    }
    return result;
}

SurfaceMetrics surface_metrics(const Manifold& solid, const SurfaceMesh& mesh) {
    const auto bounds = solid.BoundingBox();
    const auto components = solid.Decompose();
    const auto defects = inspect_surface(mesh);
    return {.points = mesh.points.size(),
            .triangles = mesh.triangles.size(),
            .connected_components = components.size(),
            .genus = solid.Genus(),
            .area = solid.SurfaceArea(),
            .volume = solid.Volume(),
            .bounds = {bounds.min.x, bounds.max.x, bounds.min.y, bounds.max.y, bounds.min.z,
                       bounds.max.z},
            .closed = solid.Status() == Manifold::Error::NoError,
            .two_manifold = defects.two_manifold,
            .boundary_edges = defects.boundary_edges,
            .incident_faces_on_non_manifold_edges =
                defects.incident_faces_on_non_manifold_edges,
            .incident_faces_on_non_manifold_vertices =
                defects.incident_faces_on_non_manifold_vertices,
            .non_manifold_edges = defects.non_manifold_edges,
            .non_manifold_vertices = defects.non_manifold_vertices,
            .holes = defects.holes,
            .unreferenced_vertices = defects.unreferenced_vertices,
            .degenerate_faces = defects.degenerate_faces,
            .duplicate_faces = defects.duplicate_faces,
            .self_intersection_pairs = defects.self_intersection_pairs,
            .self_intersecting_faces = defects.self_intersecting_faces};
}

void report_surface_quality(const GeometryCallbacks& callbacks, const double progress,
                            const std::string_view part, const SurfaceDefects& defects) {
    for (const auto& line : defects.readable_lines(part)) {
        report(callbacks, progress, line);
    }
}

void prepare_for_volume_meshing(PartGeometry& part, const GeometryCallbacks& callbacks,
                                const double progress, const std::string_view name) {
    auto defects = inspect_surface(part.surface);
    report_surface_quality(callbacks, progress, name, defects);
    if (defects.acceptable_for_volume_meshing()) {
        return;
    }
    std::ostringstream message;
    message << "[网格修复][" << name << "][进行中] CGAL PMP 正在修复 "
            << defects.self_intersection_pairs << " 对自相交三角形；相位保持不变";
    report(callbacks, progress, message.str());
    auto repaired = repair_surface(part.surface, 2);
    report_surface_quality(callbacks, progress, name, repaired.after);
    if (!repaired.accepted) {
        throw std::runtime_error{"CGAL PMP could not produce a TetGen-compatible " +
                                 std::string{name} + " surface: " + repaired.detail};
    }
    part.surface = std::move(repaired.mesh);
    part.surface_repaired = repaired.changed;
    part.repair_backend = std::move(repaired.backend);
    part.repair_volume_relative_change = repaired.volume_relative_change;
    part.surface_metrics.points = part.surface.points.size();
    part.surface_metrics.triangles = part.surface.triangles.size();
    part.surface_metrics.boundary_edges = repaired.after.boundary_edges;
    part.surface_metrics.connected_components = repaired.after.connected_components;
    part.surface_metrics.two_manifold = repaired.after.two_manifold;
    part.surface_metrics.non_manifold_edges = repaired.after.non_manifold_edges;
    part.surface_metrics.non_manifold_vertices = repaired.after.non_manifold_vertices;
    part.surface_metrics.holes = repaired.after.holes;
    part.surface_metrics.unreferenced_vertices = repaired.after.unreferenced_vertices;
    part.surface_metrics.degenerate_faces = repaired.after.degenerate_faces;
    part.surface_metrics.duplicate_faces = repaired.after.duplicate_faces;
    part.surface_metrics.self_intersection_pairs = repaired.after.self_intersection_pairs;
    part.surface_metrics.self_intersecting_faces = repaired.after.self_intersecting_faces;
    std::ostringstream success;
    success << "[网格修复][" << name
            << "][成功] 自相交与拓扑缺陷已清除；修复前后体积相对变化="
            << std::setprecision(4) << repaired.volume_relative_change * 100.0
            << "%（门限 1%），继续四面体化";
    report(callbacks, progress, success.str());
}

void remesh_part(PartGeometry& part, const domain::MeshSettings& settings,
                 const GeometryCallbacks& callbacks, const double progress,
                 const std::string_view name) {
    const auto target = settings.width * settings.target_edge_percent / 100.0;
    const auto tolerance = settings.width * settings.surface_tolerance_percent / 100.0;
    const auto minimum_edge = settings.width * settings.minimum_edge_percent / 100.0;
    const auto maximum_edge = settings.width * settings.maximum_edge_percent / 100.0;
    std::ostringstream started;
    started << "[表面重网格][" << name << "][进行中] CGAL "
            << (settings.sizing_mode == domain::SurfaceSizingMode::uniform ? "均匀" : "曲率自适应")
            << "重网格：";
    if (settings.sizing_mode == domain::SurfaceSizingMode::uniform) {
        started << "目标边长=" << std::setprecision(4) << target << " mm（RVE 的 "
                << settings.target_edge_percent << "%）";
    } else {
        started << "局部目标尺寸范围=" << std::setprecision(4) << minimum_edge << "–"
                << maximum_edge << " mm（CGAL 分裂阈值允许实际最长边约为上限的 4/3）";
    }
    started << "，迭代=" << settings.remesh_iterations
            << "，表面容差=" << tolerance << " mm，特征角="
            << settings.feature_angle_degrees << "°，锐化="
            << (settings.sharpen ? "开启" : "关闭") << "，简化="
            << (settings.simplify ? "开启" : "关闭");
    report(callbacks, progress, started.str());
    const auto before = inspect_surface(part.surface);
    report_surface_quality(callbacks, progress, name, before);
    const auto has_combinatorial_defects =
        before.boundary_edges != 0 || before.connected_components != 1 ||
        before.non_manifold_edges != 0 || before.non_manifold_vertices != 0 ||
        before.holes != 0 || before.duplicate_faces != 0;
    if (has_combinatorial_defects) {
        auto repaired = repair_surface(part.surface,
                                       static_cast<std::size_t>(settings.repair_rounds));
        if (!repaired.accepted) {
            throw std::runtime_error{"CGAL pre-remesh repair failed for " +
                                     std::string{name} + ": " + repaired.detail};
        }
        part.surface = std::move(repaired.mesh);
        part.surface_repaired = repaired.changed;
        part.repair_backend = repaired.backend;
        part.repair_volume_relative_change = repaired.volume_relative_change;
    }
    const auto pre_remesh_surface = part.surface;
    const auto remesh_options = SurfaceRemeshOptions{
        .sizing_mode = settings.sizing_mode,
        .target_edge_length = target,
        .surface_tolerance = tolerance,
        .minimum_edge_length = minimum_edge,
        .maximum_edge_length = maximum_edge,
        .iterations = settings.remesh_iterations,
        .feature_angle_degrees = settings.feature_angle_degrees,
        .sharpen = settings.sharpen,
        .simplify = settings.simplify,
        .simplify_keep_ratio = settings.simplify_keep_ratio};
    auto remeshed = remesh_surface(part.surface, remesh_options);
    part.surface = remeshed.mesh;
    auto after = inspect_surface(part.surface);
    if (!after.acceptable_for_volume_meshing()) {
        auto repaired = repair_surface(part.surface,
                                       static_cast<std::size_t>(settings.repair_rounds));
        if (repaired.accepted) {
            part.surface = std::move(repaired.mesh);
            part.surface_repaired = part.surface_repaired || repaired.changed;
            part.repair_backend = repaired.backend;
            part.repair_volume_relative_change =
                std::max(part.repair_volume_relative_change,
                         repaired.volume_relative_change);
            after = repaired.after;
        }
    }
    const auto advanced_quality_failed =
        remeshed.minimum_angle_degrees < 5.0 || remeshed.maximum_aspect_ratio > 10.0 ||
        remeshed.edge_length_cv > 0.35;
    if ((!after.acceptable_for_volume_meshing() || advanced_quality_failed) &&
        (settings.sharpen || settings.simplify)) {
        report(callbacks, progress,
               "[表面重网格][" + std::string{name} +
                   "][同相位备用路径] 高级锐化/简化造成拓扑或三角形质量门失败；"
                   "保持几何相位不变，回滚并改用回投影均匀重网格");
        auto fallback = remesh_options;
        fallback.sizing_mode = domain::SurfaceSizingMode::uniform;
        fallback.sharpen = false;
        fallback.simplify = false;
        remeshed = remesh_surface(pre_remesh_surface, fallback);
        part.surface = remeshed.mesh;
        after = inspect_surface(part.surface);
    }
    report_surface_quality(callbacks, progress, name, after);
    if (!after.acceptable_for_volume_meshing()) {
        throw std::runtime_error{"CGAL isotropic remeshing left invalid topology for " +
                                 std::string{name}};
    }
    part.surface_metrics.points = part.surface.points.size();
    part.surface_metrics.triangles = part.surface.triangles.size();
    part.surface_metrics.two_manifold = after.two_manifold;
    part.surface_metrics.boundary_edges = after.boundary_edges;
    part.surface_metrics.connected_components = after.connected_components;
    part.surface_metrics.non_manifold_edges = after.non_manifold_edges;
    part.surface_metrics.non_manifold_vertices = after.non_manifold_vertices;
    part.surface_metrics.holes = after.holes;
    part.surface_metrics.unreferenced_vertices = after.unreferenced_vertices;
    part.surface_metrics.degenerate_faces = after.degenerate_faces;
    part.surface_metrics.duplicate_faces = after.duplicate_faces;
    part.surface_metrics.self_intersection_pairs = after.self_intersection_pairs;
    part.surface_metrics.self_intersecting_faces = after.self_intersecting_faces;
    part.surface_metrics.target_edge_length = remeshed.target_edge_length;
    part.surface_metrics.minimum_triangle_angle_degrees = remeshed.minimum_angle_degrees;
    part.surface_metrics.mean_triangle_aspect_ratio = remeshed.mean_aspect_ratio;
    part.surface_metrics.maximum_triangle_aspect_ratio = remeshed.maximum_aspect_ratio;
    part.surface_metrics.edge_length_coefficient_of_variation = remeshed.edge_length_cv;
    part.surface_metrics.protected_sharp_edges = remeshed.sharp_edges;
    part.surface_metrics.surface_deviation = remeshed.surface_deviation;
    part.surface_metrics.simplified = remeshed.simplified;
    part.surface_metrics.simplification_rolled_back = remeshed.simplification_rolled_back;
    const auto deviation_within_target =
        settings.sizing_mode == domain::SurfaceSizingMode::uniform ||
        remeshed.surface_deviation <= tolerance;
    const auto quality_grade = deviation_within_target && remeshed.minimum_angle_degrees >= 20.0 &&
                                       remeshed.maximum_aspect_ratio <= 3.0 &&
                                       remeshed.edge_length_cv <= 0.25
                                   ? "优秀"
                               : deviation_within_target &&
                                         remeshed.minimum_angle_degrees >= 10.0 &&
                                         remeshed.maximum_aspect_ratio <= 5.0 &&
                                         remeshed.edge_length_cv <= 0.35
                                   ? "可用"
                                   : "需关注";
    std::ostringstream completed;
    completed << "[表面重网格][" << name << "][完成][质量等级=" << quality_grade
              << "] 三角形=" << pre_remesh_surface.triangles.size() << " → "
              << part.surface.triangles.size() << "，边长[min/mean/max]=" << std::fixed
              << std::setprecision(3) << remeshed.minimum_edge_length << '/'
              << remeshed.mean_edge_length << '/' << remeshed.maximum_edge_length
              << " mm，最小角="
              << std::setprecision(2) << remeshed.minimum_angle_degrees
              << "°，平均纵横比=" << remeshed.mean_aspect_ratio
              << "，最差纵横比=" << remeshed.maximum_aspect_ratio
              << "，边长变异系数=" << remeshed.edge_length_cv
              << "，表面偏差=" << remeshed.surface_deviation << " mm"
              << "，简化="
              << (remeshed.simplified ? "已采用" : remeshed.simplification_rolled_back
                                                       ? "已回滚"
                                                       : "未启用")
              << "（纵横比越接近 1、最小角越大、变异系数越接近 0 越好）";
    report(callbacks, progress, completed.str());
    if (!deviation_within_target) {
        std::ostringstream warning;
        warning << "[表面偏差][" << name << "][需关注] 全局近似 Hausdorff 偏差="
                << remeshed.surface_deviation << " mm，大于曲率尺寸误差目标="
                << tolerance
                << " mm。该目标用于自适应尺寸场而非全局硬约束；如需更贴近原曲面，"
                   "请减小最大边长，或开启网格锐化以保护盖板棱边。";
        report(callbacks, progress, warning.str());
    }
}

PartGeometry make_part(Manifold solid) {
    const auto status = solid.Status();
    if (status != Manifold::Error::NoError || solid.IsEmpty()) {
        throw std::runtime_error{"Manifold LevelSet failed with status " +
                                 std::to_string(static_cast<int>(status))};
    }
    PartGeometry result;
    result.surface = surface_mesh(solid);
    result.surface_metrics = surface_metrics(solid, result.surface);
    if (result.surface_metrics.connected_components != 1 || result.surface_metrics.volume <= 0.0 ||
        result.surface_metrics.area <= 0.0) {
        std::ostringstream message;
        message << "generated TPMS part failed quality gate: components="
                << result.surface_metrics.connected_components
                << ", volume=" << result.surface_metrics.volume
                << ", area=" << result.surface_metrics.area << ", component_volumes=[";
        const auto components = solid.Decompose();
        for (std::size_t index = 0; index < components.size(); ++index) {
            message << components[index].Volume();
            if (index + 1 != components.size()) {
                message << ',';
            }
        }
        message << ']';
        throw std::runtime_error{message.str()};
    }
    return result;
}

#ifdef MBS_ENABLE_TETGEN
double signed_tetrahedron_volume(const std::array<double, 3>& a, const std::array<double, 3>& b,
                                 const std::array<double, 3>& c,
                                 const std::array<double, 3>& d) {
    const std::array ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array ac{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array ad{d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    const std::array cross{ab[1] * ac[2] - ab[2] * ac[1],
                           ab[2] * ac[0] - ab[0] * ac[2],
                           ab[0] * ac[1] - ab[1] * ac[0]};
    return (cross[0] * ad[0] + cross[1] * ad[1] + cross[2] * ad[2]) / 6.0;
}

TetrahedralMetrics tetrahedral_metrics(const TetrahedralMesh& mesh) {
    TetrahedralMetrics metrics{.points = mesh.points.size(),
                               .tetrahedra = mesh.tetrahedra.size(),
                               .minimum_volume = std::numeric_limits<double>::infinity(),
                               .minimum_signed_volume = std::numeric_limits<double>::infinity()};
    for (const auto& tetrahedron : mesh.tetrahedra) {
        const auto signed_volume =
            signed_tetrahedron_volume(mesh.points[tetrahedron[0]], mesh.points[tetrahedron[1]],
                                      mesh.points[tetrahedron[2]], mesh.points[tetrahedron[3]]);
        const auto volume = std::abs(signed_volume);
        metrics.minimum_volume = std::min(metrics.minimum_volume, volume);
        metrics.minimum_signed_volume = std::min(metrics.minimum_signed_volume, signed_volume);
        metrics.non_positive_count += signed_volume <= 0.0 ? 1U : 0U;
        metrics.small_volume_count += volume <= small_tetrahedron_volume ? 1U : 0U;
    }
    if (mesh.tetrahedra.empty()) {
        metrics.minimum_volume = 0.0;
        metrics.minimum_signed_volume = 0.0;
    }
    return metrics;
}

TetrahedralMesh tetrahedralize(const SurfaceMesh& surface,
                               const domain::TetgenSettings& settings) {
    tetgenio input;
    tetgenio output;
    input.firstnumber = 0;
    input.numberofpoints = static_cast<int>(surface.points.size());
    input.pointlist = new REAL[static_cast<std::size_t>(input.numberofpoints) * 3];
    for (std::size_t index = 0; index < surface.points.size(); ++index) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            input.pointlist[index * 3 + axis] = surface.points[index][axis];
        }
    }
    input.numberoffacets = static_cast<int>(surface.triangles.size());
    input.facetlist = new tetgenio::facet[static_cast<std::size_t>(input.numberoffacets)];
    input.facetmarkerlist = new int[static_cast<std::size_t>(input.numberoffacets)];
    for (std::size_t index = 0; index < surface.triangles.size(); ++index) {
        auto& facet = input.facetlist[index];
        facet.numberofpolygons = 1;
        facet.polygonlist = new tetgenio::polygon[1];
        facet.numberofholes = 0;
        facet.holelist = nullptr;
        auto& polygon = facet.polygonlist[0];
        polygon.numberofvertices = 3;
        polygon.vertexlist = new int[3];
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            polygon.vertexlist[vertex] = static_cast<int>(surface.triangles[index][vertex]);
        }
        input.facetmarkerlist[index] = 1;
    }

    std::ostringstream switches;
    switches << "pzQ";
    if (settings.quality) {
        switches << 'q' << std::setprecision(12) << settings.minimum_ratio << '/'
                 << settings.minimum_dihedral;
    }
    if (settings.no_bisect) {
        switches << 'Y';
    }
    if (settings.target_edge_length_mm > 0.0) {
        const auto maximum_volume =
            std::pow(settings.target_edge_length_mm, 3.0) / (6.0 * std::sqrt(2.0));
        switches << 'a' << std::setprecision(12) << maximum_volume;
    }
    switches << 'O' << settings.optimization_level;
    if (settings.order == 2) {
        switches << "o2";
    } else {
        // TetGen 1.6's internal consistency checker interprets the six
        // quadratic connectivity slots as topology pointers and emits a huge
        // false-positive report after -o2.  The C++ postconditions below
        // validate Tet10 indices and edge midpoints explicitly instead.
        switches << 'C';
    }
    auto option_text = switches.str();
    option_text.push_back('\0');
    try {
        ::tetrahedralize(option_text.data(), &input, &output);
    } catch (const int code) {
        throw std::runtime_error{"TetGen failed with error code " + std::to_string(code)};
    }
    const auto expected_corners = settings.order == 2 ? 10 : 4;
    if (output.numberofpoints <= 0 || output.numberoftetrahedra <= 0 ||
        output.numberofcorners < expected_corners) {
        throw std::runtime_error{"TetGen returned an empty tetrahedral mesh"};
    }

    TetrahedralMesh result;
    result.order = settings.order;
    result.points.reserve(static_cast<std::size_t>(output.numberofpoints));
    for (int index = 0; index < output.numberofpoints; ++index) {
        const std::array point{static_cast<double>(output.pointlist[index * 3]),
                               static_cast<double>(output.pointlist[index * 3 + 1]),
                               static_cast<double>(output.pointlist[index * 3 + 2])};
        if (!std::ranges::all_of(point, [](const double value) { return std::isfinite(value); })) {
            throw std::runtime_error{"TetGen returned a non-finite point"};
        }
        result.points.push_back(point);
    }
    result.tetrahedra.reserve(static_cast<std::size_t>(output.numberoftetrahedra));
    for (int index = 0; index < output.numberoftetrahedra; ++index) {
        const auto offset = static_cast<std::size_t>(index * output.numberofcorners);
        std::array<std::uint32_t, 10> tetrahedron{};
        for (std::size_t vertex = 0; vertex < 4; ++vertex) {
            const auto value = output.tetrahedronlist[offset + vertex];
            if (value < 0 || value >= output.numberofpoints) {
                throw std::runtime_error{"TetGen returned an out-of-range tetrahedron index"};
            }
            tetrahedron[vertex] = static_cast<std::uint32_t>(value);
        }
        if (signed_tetrahedron_volume(result.points[tetrahedron[0]], result.points[tetrahedron[1]],
                                      result.points[tetrahedron[2]],
                                      result.points[tetrahedron[3]]) < 0.0) {
            std::swap(tetrahedron[0], tetrahedron[1]);
        }
        if (settings.order == 2) {
            static constexpr std::array<std::array<std::size_t, 2>, 6> abaqus_edges{
                {{{0, 1}}, {{1, 2}}, {{2, 0}}, {{0, 3}}, {{1, 3}}, {{2, 3}}}};
            std::array<bool, 6> used{};
            for (std::size_t edge = 0; edge < abaqus_edges.size(); ++edge) {
                const auto& a = result.points[tetrahedron[abaqus_edges[edge][0]]];
                const auto& b = result.points[tetrahedron[abaqus_edges[edge][1]]];
                const std::array midpoint{(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5,
                                          (a[2] + b[2]) * 0.5};
                std::size_t best = 6;
                double best_distance = std::numeric_limits<double>::infinity();
                for (std::size_t candidate = 0; candidate < 6; ++candidate) {
                    if (used[candidate]) {
                        continue;
                    }
                    const auto value = output.tetrahedronlist[offset + 4 + candidate];
                    if (value < 0 || value >= output.numberofpoints) {
                        throw std::runtime_error{
                            "TetGen returned an out-of-range quadratic node index"};
                    }
                    const auto& point = result.points[static_cast<std::size_t>(value)];
                    const auto distance = std::hypot(point[0] - midpoint[0],
                                                     point[1] - midpoint[1],
                                                     point[2] - midpoint[2]);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = candidate;
                    }
                }
                const auto scale = std::max(std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]),
                                            1.0);
                if (best == 6 || best_distance > scale * 1.0e-8) {
                    throw std::runtime_error{
                        "TetGen quadratic nodes do not match Tet10 edge midpoints"};
                }
                used[best] = true;
                tetrahedron[4 + edge] = static_cast<std::uint32_t>(
                    output.tetrahedronlist[offset + 4 + best]);
            }
        }
        result.tetrahedra.push_back(tetrahedron);
    }
    const auto metrics = tetrahedral_metrics(result);
    if (metrics.non_positive_count != 0 || metrics.small_volume_count != 0) {
        throw std::runtime_error{"TetGen output contains non-positive or near-zero elements"};
    }
    return result;
}
#endif

void write_obj(const std::filesystem::path& path, const SurfaceMesh& mesh) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write OBJ: " + path.string()};
    }
    stream << std::setprecision(17);
    for (const auto& point : mesh.points) {
        stream << "v " << point[0] << ' ' << point[1] << ' ' << point[2] << '\n';
    }
    for (const auto& triangle : mesh.triangles) {
        stream << "f " << triangle[0] + 1 << ' ' << triangle[1] + 1 << ' ' << triangle[2] + 1
               << '\n';
    }
    if (!stream) {
        throw std::runtime_error{"failed while writing OBJ: " + path.string()};
    }
}

void write_surface_vtp(const std::filesystem::path& path, const SurfaceMesh& mesh) {
#ifdef MBS_ENABLE_VTK_IO
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    points->SetNumberOfPoints(static_cast<vtkIdType>(mesh.points.size()));
    for (std::size_t index = 0; index < mesh.points.size(); ++index) {
        points->SetPoint(static_cast<vtkIdType>(index), mesh.points[index].data());
    }
    vtkNew<vtkCellArray> polygons;
    for (const auto& triangle : mesh.triangles) {
        const vtkIdType ids[]{static_cast<vtkIdType>(triangle[0]),
                              static_cast<vtkIdType>(triangle[1]),
                              static_cast<vtkIdType>(triangle[2])};
        polygons->InsertNextCell(3, ids);
    }
    vtkNew<vtkPolyData> data;
    data->SetPoints(points);
    data->SetPolys(polygons);
    vtkNew<vtkXMLPolyDataWriter> writer;
    writer->SetFileName(path.string().c_str());
    writer->SetInputData(data);
    writer->SetDataModeToBinary();
    writer->SetCompressorTypeToZLib();
    if (writer->Write() != 1) {
        throw std::runtime_error{"cannot write compressed VTP: " + path.string()};
    }
    return;
#else
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write VTP: " + path.string()};
    }
    stream << std::setprecision(17)
           << "<?xml version=\"1.0\"?>\n<VTKFile type=\"PolyData\" version=\"0.1\" "
              "byte_order=\"LittleEndian\">\n<PolyData>\n<Piece NumberOfPoints=\""
           << mesh.points.size() << "\" NumberOfVerts=\"0\" NumberOfLines=\"0\" "
              "NumberOfStrips=\"0\" NumberOfPolys=\""
           << mesh.triangles.size() << "\">\n<Points>\n<DataArray type=\"Float64\" "
              "NumberOfComponents=\"3\" format=\"ascii\">\n";
    for (const auto& point : mesh.points) {
        stream << point[0] << ' ' << point[1] << ' ' << point[2] << '\n';
    }
    stream << "</DataArray>\n</Points>\n<Polys>\n<DataArray type=\"Int64\" "
              "Name=\"connectivity\" format=\"ascii\">\n";
    for (const auto& triangle : mesh.triangles) {
        stream << triangle[0] << ' ' << triangle[1] << ' ' << triangle[2] << '\n';
    }
    stream << "</DataArray>\n<DataArray type=\"Int64\" Name=\"offsets\" "
              "format=\"ascii\">\n";
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        stream << (index + 1) * 3 << ' ';
    }
    stream << "\n</DataArray>\n</Polys>\n</Piece>\n</PolyData>\n</VTKFile>\n";
    if (!stream) {
        throw std::runtime_error{"failed while writing VTP: " + path.string()};
    }
#endif
}

SurfaceMesh box_surface(const double width, const double depth, const double height,
                        const double center_z) {
    const double x = width / 2.0;
    const double y = depth / 2.0;
    const double z = height / 2.0;
    return {.points = {{{-x, -y, center_z - z}}, {{x, -y, center_z - z}},
                       {{x, y, center_z - z}},   {{-x, y, center_z - z}},
                       {{-x, -y, center_z + z}}, {{x, -y, center_z + z}},
                       {{x, y, center_z + z}},   {{-x, y, center_z + z}}},
            .triangles = {{{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
                           {{0, 1, 5}}, {{0, 5, 4}}, {{1, 2, 6}}, {{1, 6, 5}},
                           {{2, 3, 7}}, {{2, 7, 6}}, {{3, 0, 4}}, {{3, 4, 7}}}};
}

[[maybe_unused]] void write_sampling_grid_vtk(const std::filesystem::path& path, const double width,
                             const double total_height, const double spacing) {
    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{"cannot write sampling-grid VTK: " + path.string()};
    }
    // This is the actual scalar-sampling lattice shown to the user.  Keep an
    // isotropic interval equal to the LevelSet edge length on all three axes;
    // unlike the former diagnostic preview, it is not independently decimated.
    const int xy_divisions = std::max(1, static_cast<int>(std::floor(width / spacing)));
    const int z_divisions =
        std::max(1, static_cast<int>(std::floor(total_height / spacing)));
    const int x_lines = (xy_divisions + 1) * (z_divisions + 1);
    const int y_lines = x_lines;
    const int z_lines = (xy_divisions + 1) * (xy_divisions + 1);
    const int line_count = x_lines + y_lines + z_lines;
    const int point_count = line_count * 2;
    stream << "# vtk DataFile Version 3.0\nMBS 4.0 sampling lattice\nASCII\nDATASET POLYDATA\n"
           << "POINTS " << point_count << " double\n";
    const auto coordinate = [spacing](const int index, const int divisions) {
        return -spacing * static_cast<double>(divisions) / 2.0 +
               spacing * static_cast<double>(index);
    };
    const auto x_min = coordinate(0, xy_divisions);
    const auto x_max = coordinate(xy_divisions, xy_divisions);
    const auto z_min = coordinate(0, z_divisions);
    const auto z_max = coordinate(z_divisions, z_divisions);
    for (int y = 0; y <= xy_divisions; ++y) {
        for (int z = 0; z <= z_divisions; ++z) {
            stream << x_min << ' ' << coordinate(y, xy_divisions) << ' '
                   << coordinate(z, z_divisions) << '\n'
                   << x_max << ' ' << coordinate(y, xy_divisions) << ' '
                   << coordinate(z, z_divisions) << '\n';
        }
    }
    for (int x = 0; x <= xy_divisions; ++x) {
        for (int z = 0; z <= z_divisions; ++z) {
            stream << coordinate(x, xy_divisions) << ' ' << x_min << ' '
                   << coordinate(z, z_divisions) << '\n'
                   << coordinate(x, xy_divisions) << ' ' << x_max << ' '
                   << coordinate(z, z_divisions) << '\n';
        }
    }
    for (int x = 0; x <= xy_divisions; ++x) {
        for (int y = 0; y <= xy_divisions; ++y) {
            stream << coordinate(x, xy_divisions) << ' '
                   << coordinate(y, xy_divisions) << ' ' << z_min << '\n'
                   << coordinate(x, xy_divisions) << ' '
                   << coordinate(y, xy_divisions) << ' ' << z_max << '\n';
        }
    }
    stream << "LINES " << line_count << ' ' << line_count * 3 << '\n';
    for (int line = 0; line < line_count; ++line) {
        stream << "2 " << line * 2 << ' ' << line * 2 + 1 << '\n';
    }
}

void write_sampling_grid_vti(const std::filesystem::path& path, const double width,
                             const double total_height, const double spacing) {
    const int xy = std::max(1, static_cast<int>(std::floor(width / spacing)));
    const int z = std::max(1, static_cast<int>(std::floor(total_height / spacing)));
    const auto origin_x = -spacing * static_cast<double>(xy) / 2.0;
    const auto origin_z = -spacing * static_cast<double>(z) / 2.0;
#ifdef MBS_ENABLE_VTK_IO
    vtkNew<vtkImageData> data;
    data->SetDimensions(xy + 1, xy + 1, z + 1);
    data->SetOrigin(origin_x, origin_x, origin_z);
    data->SetSpacing(spacing, spacing, spacing);
    vtkNew<vtkXMLImageDataWriter> writer;
    writer->SetFileName(path.string().c_str());
    writer->SetInputData(data);
    writer->SetDataModeToBinary();
    writer->SetCompressorTypeToZLib();
    if (writer->Write() != 1) {
        throw std::runtime_error{"cannot write compressed VTI: " + path.string()};
    }
    return;
#else
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write sampling-grid VTI: " + path.string()};
    }
    stream << std::setprecision(17)
           << "<?xml version=\"1.0\"?>\n<VTKFile type=\"ImageData\" version=\"0.1\" "
              "byte_order=\"LittleEndian\">\n<ImageData WholeExtent=\"0 "
           << xy << " 0 " << xy << " 0 " << z << "\" Origin=\"" << origin_x << ' '
           << origin_x << ' ' << origin_z << "\" Spacing=\"" << spacing << ' ' << spacing
           << ' ' << spacing << "\">\n<Piece Extent=\"0 " << xy << " 0 " << xy << " 0 " << z
           << "\">\n<PointData/>\n<CellData/>\n</Piece>\n</ImageData>\n</VTKFile>\n";
#endif
}

TetrahedralMesh clipped_tetrahedra(const TetrahedralMesh& mesh, const bool positive) {
    TetrahedralMesh result;
    result.points = mesh.points;
    result.order = mesh.order;
    for (const auto& tetrahedron : mesh.tetrahedra) {
        double centroid_y = 0.0;
        for (std::size_t vertex = 0; vertex < 4; ++vertex) {
            centroid_y += mesh.points[tetrahedron[vertex]][1];
        }
        if ((centroid_y >= 0.0) == positive) {
            result.tetrahedra.push_back(tetrahedron);
        }
    }
    return result;
}

void write_surface_inp(const std::filesystem::path& path, const SurfaceMesh& mesh,
                       const std::string_view part_name) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write surface INP: " + path.string()};
    }
    stream << "*Heading\n** MBS 4.0 C++ surface mesh\n*Part, name=" << part_name
           << "\n*Node\n" << std::setprecision(17);
    for (std::size_t index = 0; index < mesh.points.size(); ++index) {
        const auto& point = mesh.points[index];
        stream << index + 1 << ", " << point[0] << ", " << point[1] << ", " << point[2]
               << '\n';
    }
    stream << "*Element, type=S3, elset=EALL\n";
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        const auto& triangle = mesh.triangles[index];
        stream << index + 1 << ", " << triangle[0] + 1 << ", " << triangle[1] + 1 << ", "
               << triangle[2] + 1 << '\n';
    }
    stream << "*Surface, type=ELEMENT, name=ALL_SURF\nEALL, SPOS\n*End Part\n";
}

struct BoundaryFace final {
    std::size_t element{};
    int local_face{};
    int count{};
};

void write_id_list(std::ostream& stream, const std::vector<std::size_t>& ids) {
    for (std::size_t index = 0; index < ids.size(); ++index) {
        stream << ids[index];
        if (index + 1 == ids.size() || (index + 1) % 16 == 0) {
            stream << '\n';
        } else {
            stream << ", ";
        }
    }
}

void write_tetrahedral_inp(const std::filesystem::path& path, const TetrahedralMesh& mesh,
                           const std::string_view part_name) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write tetrahedral INP: " + path.string()};
    }
    stream << "*Heading\n** MBS 4.0 C++ TetGen mesh\n*Part, name=" << part_name
           << "\n*Node\n" << std::setprecision(17);
    for (std::size_t index = 0; index < mesh.points.size(); ++index) {
        const auto& point = mesh.points[index];
        stream << index + 1 << ", " << point[0] << ", " << point[1] << ", " << point[2]
               << '\n';
    }
    const auto nodes_per_element = mesh.nodes_per_element();
    stream << "*Element, type=" << (mesh.order == 2 ? "C3D10" : "C3D4")
           << ", elset=EALL\n";
    for (std::size_t index = 0; index < mesh.tetrahedra.size(); ++index) {
        const auto& tetrahedron = mesh.tetrahedra[index];
        stream << index + 1;
        for (std::size_t vertex = 0; vertex < nodes_per_element; ++vertex) {
            stream << ", " << tetrahedron[vertex] + 1;
        }
        stream << '\n';
    }

    const std::array<std::array<int, 3>, 4> local_faces{{{0, 1, 2}, {0, 3, 1},
                                                         {1, 3, 2}, {2, 3, 0}}};
    std::map<std::array<std::uint32_t, 3>, BoundaryFace> faces;
    for (std::size_t element = 0; element < mesh.tetrahedra.size(); ++element) {
        const auto& tetrahedron = mesh.tetrahedra[element];
        for (std::size_t face_index = 0; face_index < local_faces.size(); ++face_index) {
            std::array<std::uint32_t, 3> key{
                tetrahedron[static_cast<std::size_t>(local_faces[face_index][0])],
                tetrahedron[static_cast<std::size_t>(local_faces[face_index][1])],
                tetrahedron[static_cast<std::size_t>(local_faces[face_index][2])]};
            std::ranges::sort(key);
            auto [position, inserted] = faces.try_emplace(
                key, BoundaryFace{.element = element + 1,
                                  .local_face = static_cast<int>(face_index + 1),
                                  .count = 1});
            if (!inserted) {
                ++position->second.count;
            }
        }
    }
    std::array<std::vector<std::size_t>, 4> boundary_sets;
    for (const auto& [unused, face] : faces) {
        static_cast<void>(unused);
        if (face.count == 1) {
            boundary_sets[static_cast<std::size_t>(face.local_face - 1)].push_back(face.element);
        }
    }
    for (std::size_t face = 0; face < boundary_sets.size(); ++face) {
        if (!boundary_sets[face].empty()) {
            stream << "*Elset, elset=SURF_S" << face + 1 << '\n';
            write_id_list(stream, boundary_sets[face]);
        }
    }
    stream << "*Surface, type=ELEMENT, name=ALL_SURF\n";
    for (std::size_t face = 0; face < boundary_sets.size(); ++face) {
        if (!boundary_sets[face].empty()) {
            stream << "SURF_S" << face + 1 << ", S" << face + 1 << '\n';
        }
    }
    stream << "*End Part\n";
}

[[maybe_unused]] void write_tetrahedral_vtk(const std::filesystem::path& path,
                           const TetrahedralMesh& mesh) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write tetrahedral VTK: " + path.string()};
    }
    stream << "# vtk DataFile Version 3.0\nMBS 4.0 TetGen volume mesh\nASCII\n"
              "DATASET UNSTRUCTURED_GRID\nPOINTS "
           << mesh.points.size() << " double\n" << std::setprecision(17);
    for (const auto& point : mesh.points) {
        stream << point[0] << ' ' << point[1] << ' ' << point[2] << '\n';
    }
    const auto nodes_per_element = mesh.nodes_per_element();
    stream << "CELLS " << mesh.tetrahedra.size() << ' '
           << mesh.tetrahedra.size() * (nodes_per_element + 1)
           << '\n';
    for (const auto& tetrahedron : mesh.tetrahedra) {
        stream << nodes_per_element;
        for (std::size_t vertex = 0; vertex < nodes_per_element; ++vertex) {
            stream << ' ' << tetrahedron[vertex];
        }
        stream << '\n';
    }
    stream << "CELL_TYPES " << mesh.tetrahedra.size() << '\n';
    for (std::size_t index = 0; index < mesh.tetrahedra.size(); ++index) {
        stream << (mesh.order == 2 ? 24 : 10) << '\n';
    }
    if (!stream) {
        throw std::runtime_error{"failed while writing tetrahedral VTK: " + path.string()};
    }
}

void write_tetrahedral_vtu(const std::filesystem::path& path,
                           const TetrahedralMesh& mesh) {
#ifdef MBS_ENABLE_VTK_IO
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    points->SetNumberOfPoints(static_cast<vtkIdType>(mesh.points.size()));
    for (std::size_t index = 0; index < mesh.points.size(); ++index) {
        points->SetPoint(static_cast<vtkIdType>(index), mesh.points[index].data());
    }
    vtkNew<vtkCellArray> cells;
    const auto nodes = mesh.nodes_per_element();
    for (const auto& tetrahedron : mesh.tetrahedra) {
        std::array<vtkIdType, 10> ids{};
        for (std::size_t index = 0; index < nodes; ++index) {
            ids[index] = static_cast<vtkIdType>(tetrahedron[index]);
        }
        cells->InsertNextCell(static_cast<vtkIdType>(nodes), ids.data());
    }
    vtkNew<vtkUnstructuredGrid> data;
    data->SetPoints(points);
    data->SetCells(mesh.order == 2 ? 24 : 10, cells);
    vtkNew<vtkXMLUnstructuredGridWriter> writer;
    writer->SetFileName(path.string().c_str());
    writer->SetInputData(data);
    writer->SetDataModeToBinary();
    writer->SetCompressorTypeToZLib();
    if (writer->Write() != 1) {
        throw std::runtime_error{"cannot write compressed VTU: " + path.string()};
    }
    return;
#else
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write tetrahedral VTU: " + path.string()};
    }
    const auto nodes = mesh.nodes_per_element();
    stream << std::setprecision(17)
           << "<?xml version=\"1.0\"?>\n<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" "
              "byte_order=\"LittleEndian\">\n<UnstructuredGrid>\n<Piece NumberOfPoints=\""
           << mesh.points.size() << "\" NumberOfCells=\"" << mesh.tetrahedra.size()
           << "\">\n<Points><DataArray type=\"Float64\" NumberOfComponents=\"3\" "
              "format=\"ascii\">\n";
    for (const auto& point : mesh.points) {
        stream << point[0] << ' ' << point[1] << ' ' << point[2] << '\n';
    }
    stream << "</DataArray></Points>\n<Cells>\n<DataArray type=\"Int64\" "
              "Name=\"connectivity\" format=\"ascii\">\n";
    for (const auto& tet : mesh.tetrahedra) {
        for (std::size_t index = 0; index < nodes; ++index) stream << tet[index] << ' ';
        stream << '\n';
    }
    stream << "</DataArray>\n<DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">\n";
    for (std::size_t index = 0; index < mesh.tetrahedra.size(); ++index)
        stream << (index + 1) * nodes << ' ';
    stream << "\n</DataArray>\n<DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
    for (std::size_t index = 0; index < mesh.tetrahedra.size(); ++index)
        stream << (mesh.order == 2 ? 24 : 10) << ' ';
    stream << "\n</DataArray>\n</Cells>\n</Piece>\n</UnstructuredGrid>\n</VTKFile>\n";
    if (!stream) {
        throw std::runtime_error{"failed while writing VTU: " + path.string()};
    }
#endif
}

std::string part_json(const PartGeometry& part) {
    const auto& metrics = part.surface_metrics;
    std::ostringstream stream;
    stream << std::setprecision(15) << "{\"points\":" << metrics.points
           << ",\"triangles\":" << metrics.triangles << ",\"connected_components\":"
           << metrics.connected_components << ",\"genus\":" << metrics.genus
           << ",\"area\":" << metrics.area << ",\"volume\":" << metrics.volume
           << ",\"bounds\":[" << metrics.bounds[0] << ',' << metrics.bounds[1] << ','
           << metrics.bounds[2] << ',' << metrics.bounds[3] << ',' << metrics.bounds[4] << ','
           << metrics.bounds[5] << "]" << ",\"closed\":"
           << (metrics.closed ? "true" : "false") << ",\"is_manifold\":"
           << (metrics.two_manifold ? "true" : "false")
           << ",\"boundary_edges\":" << metrics.boundary_edges
           << ",\"incident_faces_on_non_manifold_edges\":"
           << metrics.incident_faces_on_non_manifold_edges
           << ",\"incident_faces_on_non_manifold_vertices\":"
           << metrics.incident_faces_on_non_manifold_vertices
           << ",\"non_manifold_edges\":" << metrics.non_manifold_edges
           << ",\"non_manifold_vertices\":" << metrics.non_manifold_vertices
           << ",\"holes\":" << metrics.holes
           << ",\"unreferenced_vertices\":" << metrics.unreferenced_vertices
           << ",\"degenerate_faces\":" << metrics.degenerate_faces
           << ",\"duplicate_faces\":" << metrics.duplicate_faces
           << ",\"self_intersection_pairs\":" << metrics.self_intersection_pairs
           << ",\"self_intersecting_faces\":" << metrics.self_intersecting_faces
           << ",\"target_edge_length\":" << metrics.target_edge_length
           << ",\"minimum_triangle_angle_degrees\":"
           << metrics.minimum_triangle_angle_degrees
           << ",\"mean_triangle_aspect_ratio\":" << metrics.mean_triangle_aspect_ratio
           << ",\"maximum_triangle_aspect_ratio\":" << metrics.maximum_triangle_aspect_ratio
           << ",\"edge_length_coefficient_of_variation\":"
           << metrics.edge_length_coefficient_of_variation
           << ",\"protected_sharp_edges\":" << metrics.protected_sharp_edges
           << ",\"surface_deviation\":" << metrics.surface_deviation
           << ",\"simplified\":" << (metrics.simplified ? "true" : "false")
           << ",\"simplification_rolled_back\":"
           << (metrics.simplification_rolled_back ? "true" : "false")
           << ",\"surface_repaired\":" << (part.surface_repaired ? "true" : "false")
           << ",\"repair_backend\":\"" << part.repair_backend << "\"";
    stream << ",\"repair_volume_relative_change\":"
           << part.repair_volume_relative_change;
    if (part.volume_metrics) {
        const auto& volume = *part.volume_metrics;
        stream << ",\"tetrahedral\":{\"points\":" << volume.points
               << ",\"tetrahedra\":" << volume.tetrahedra
               << ",\"order\":" << part.volume->order
               << ",\"minimum_volume\":" << volume.minimum_volume
               << ",\"minimum_signed_volume\":" << volume.minimum_signed_volume
               << ",\"non_positive_count\":" << volume.non_positive_count
               << ",\"small_volume_count\":" << volume.small_volume_count << '}';
    }
    stream << '}';
    return stream.str();
}

void write_metadata(const std::filesystem::path& path, const GeometryResult& result,
                    const domain::DesignConfig& config) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write geometry metadata: " + path.string()};
    }
    stream << std::setprecision(15)
           << "{\n  \"schema_version\": 2,\n  \"generator\": \"mbs-4.0-cpp\",\n"
           << "  \"surface_backend\": \"" << result.surface_backend << "\",\n"
           << "  \"tetrahedral_backend\": \"" << result.tetrahedral_backend << "\",\n"
           << "  \"lmd\": " << config.parameters.lambda << ",\n  \"mu\": "
           << config.parameters.mu << ",\n  \"kpa\": " << config.parameters.kappa
           << ",\n  \"bta\": " << config.parameters.beta << ",\n  \"rnd_x\": "
           << result.phase_x << ",\n  \"rnd_y\": " << result.phase_y << ",\n  \"wth\": "
           << config.mesh.width << ",\n  \"rep_z\": " << config.mesh.repeat_z
           << ",\n  \"thk_p\": " << config.mesh.plate_thickness << ",\n  \"m\": "
           << config.mesh.resolution << ",\n  \"len_pct\": "
           << config.mesh.target_edge_percent << ",\n  \"grid_spacing\": " << result.grid_spacing
           << ",\n  \"sizing_mode\": \"" << domain::to_string(config.mesh.sizing_mode) << "\""
           << ",\n  \"surface_tolerance_percent\": "
           << config.mesh.surface_tolerance_percent
           << ",\n  \"minimum_edge_percent\": " << config.mesh.minimum_edge_percent
           << ",\n  \"maximum_edge_percent\": " << config.mesh.maximum_edge_percent
           << ",\n  \"remesh_iterations\": " << config.mesh.remesh_iterations
           << ",\n  \"feature_angle_degrees\": " << config.mesh.feature_angle_degrees
           << ",\n  \"sharpen\": " << (config.mesh.sharpen ? "true" : "false")
           << ",\n  \"simplify\": " << (config.mesh.simplify ? "true" : "false")
           << ",\n  \"simplify_keep_ratio\": " << config.mesh.simplify_keep_ratio
           << ",\n  \"repair_rounds\": " << config.mesh.repair_rounds
           << ",\n  \"resample_phase\": " << (config.random_phase ? "true" : "false")
           << ",\n  \"part_b_construction\": \""
           << domain::to_string(config.part_b_construction) << "\""
           << ",\n  \"maximum_attempts\": " << config.mesh.max_attempts
           << ",\n  \"tet_order\": " << config.mesh.tetgen.order
           << ",\n  \"tet_target_edge_length_mm\": "
           << config.mesh.tetgen.target_edge_length_mm
           << ",\n  \"tet_optimization_level\": "
           << config.mesh.tetgen.optimization_level
           << ",\n  \"tet_minimum_dihedral_degrees\": "
           << config.mesh.tetgen.minimum_dihedral
           << ",\n  \"tet_maximum_radius_edge_ratio\": "
           << config.mesh.tetgen.minimum_ratio
           << ",\n  \"tet_preserve_input_surface\": "
           << (config.mesh.tetgen.no_bisect ? "true" : "false")
           << ",\n  \"tet_quality_refinement\": "
           << (config.mesh.tetgen.quality ? "true" : "false")
           << ",\n  \"tet_generated\": "
           << (config.mesh.tetrahedralize ? "true" : "false") << ",\n"
           << "  \"files\": {\"tri_a\": \"tpms-tri-A.inp\", \"tri_b\": "
              "\"tpms-tri-B.inp\", \"tet_a\": \""
           << (config.mesh.tetrahedralize ? "tpms-tet-A.inp" : "")
           << "\", \"tet_b\": \""
           << (config.mesh.tetrahedralize ? "tpms-tet-B.inp" : "")
           << "\", \"visualization\": \"visualization\"},\n"
           << "  \"metrics\": " << metrics_json(result) << "\n}\n";
}

} // namespace

double GeometryKernel::implicit_field(const double x, const double y, const double z,
                                      const domain::DesignParameters& parameters,
                                      const double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        throw std::invalid_argument{"TPMS scale must be finite and positive"};
    }
    const auto sx = std::sin(x / scale);
    const auto sy = std::sin(y / scale);
    const auto sz = std::sin(z / scale);
    const auto cx = std::cos(x / scale);
    const auto cy = std::cos(y / scale);
    const auto cz = std::cos(z / scale);
    const auto c2x = std::cos(2.0 * x / scale);
    const auto c2y = std::cos(2.0 * y / scale);
    const auto c2z = std::cos(2.0 * z / scale);
    const auto gyroid = sx * cy + sy * cz + sz * cx;
    const auto diamond = cx * cy * cz - sx * sy * sz;
    const auto fks = c2x * sy * cz + cx * c2y * sz + sx * cy * c2z;
    return parameters.lambda * gyroid + parameters.mu * diamond +
           (1.0 - parameters.lambda - parameters.mu) * fks +
           (parameters.kappa * (z / scale) + parameters.beta);
}

GeometryResult GeometryKernel::generate(const domain::DesignConfig& config,
                                        const GeometryCallbacks& callbacks) const {
    const auto validation = config.validation_errors();
    if (!validation.empty()) {
        throw std::invalid_argument{"invalid design configuration: " + validation.front()};
    }
    report(callbacks, 0.02, "validated C++ design configuration");
    const auto width = config.mesh.width;
    const auto half_width = width / 2.0;
    const auto half_height = width * static_cast<double>(config.mesh.repeat_z) / 2.0;
    const auto plate = config.mesh.plate_thickness;
    const auto spacing = width / static_cast<double>(config.mesh.resolution);
    // Avoid aligning the marching-tetrahedra lattice exactly with periodic TPMS
    // symmetry planes.  Exact alignment can create a geometrically coincident
    // (though topologically manifold) vertex pair that constrained TetGen rejects.
    const auto extraction_edge = spacing * 0.997;
    const auto scale = width / (2.0 * std::numbers::pi);
    // Follow the proven 3.0 construction order: extract the implicit phases in
    // an oversized middle domain, add oversized plates, then crop with exact
    // target boxes.  The final planar boundaries therefore come from Manifold
    // box booleans instead of a sampled LevelSet approximation at the target
    // boundary.
    constexpr double middle_scale = 1.5;
    constexpr double outer_scale = middle_scale * middle_scale;
    const auto middle_half_width = half_width * middle_scale;
    const auto middle_half_height = half_height + plate;
    const auto outer_half_width = half_width * outer_scale;
    const auto outer_half_height = middle_half_height + plate;
    const auto extraction_bounds =
        Box{vec3(-outer_half_width, -outer_half_width, -outer_half_height),
            vec3(outer_half_width, outer_half_width, outer_half_height)};

    std::mt19937_64 generator{std::random_device{}()};
    std::uniform_real_distribution<double> phase_distribution{0.0, 1.0};
    std::string last_error;
    const auto attempts = config.random_phase ? config.mesh.max_attempts : 1;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        const auto phase_x = config.random_phase ? phase_distribution(generator)
                                                  : config.parameters.phase_x;
        const auto phase_y = config.random_phase ? phase_distribution(generator)
                                                  : config.parameters.phase_y;
        try {
            const auto lattice_field = [&](const vec3 point) {
                auto parameters = config.parameters;
                const auto shifted_x = point.x + 2.0 * std::numbers::pi * phase_x;
                const auto shifted_y = point.y + 2.0 * std::numbers::pi * phase_y;
                return implicit_field(shifted_x, shifted_y, point.z, parameters, scale);
            };
            // Give the plates a sub-cell overlap with the lattice domain.  A merely
            // coplanar max() union leaves an all-zero sampling sheet and may be split
            // into two components by marching tetrahedra.
            const auto plate_overlap = spacing * 0.25;
            report(callbacks, 0.08, "extracting Part A with Manifold LevelSet");
            auto sampled_phase = Manifold::LevelSet(
                [&](const vec3 point) { return -lattice_field(point); }, extraction_bounds,
                extraction_edge, 0.0, spacing * 1.0e-5, false);
            const auto middle_solid =
                Manifold::Cube(vec3(2.0 * middle_half_width, 2.0 * middle_half_width,
                                    2.0 * middle_half_height),
                               true);
            auto lattice_a = sampled_phase ^ middle_solid;
            auto raw_surface_a = surface_mesh(lattice_a);
            auto lower_plate =
                Manifold::Cube(vec3(width * outer_scale, width * outer_scale,
                                    2.0 * plate + plate_overlap),
                               true)
                    .Translate(vec3(0.0, 0.0,
                                    -half_height - plate + plate_overlap / 2.0));
            auto plate_union_a = lattice_a + lower_plate;
            auto final_crop_a =
                Manifold::Cube(vec3(width, width, 2.0 * half_height + plate), true)
                    .Translate(vec3(0.0, 0.0, -plate / 2.0));
            auto boolean_a = plate_union_a ^ final_crop_a;
            auto plate_union_surface_a = surface_mesh(plate_union_a);
            auto boolean_surface_a = surface_mesh(boolean_a);
            auto part_a = make_part(boolean_a);
            part_a.raw_surface = std::move(raw_surface_a);
            part_a.plate_union_surface = std::move(plate_union_surface_a);
            part_a.boolean_surface = std::move(boolean_surface_a);
            remesh_part(part_a, config.mesh, callbacks, 0.30, "Part A");
            report(callbacks, 0.43, "Part A surface remeshing passed the two-manifold gate");

            report(callbacks, 0.44,
                   "constructing complementary Part B from the shared TPMS interface");
            auto lattice_b = middle_solid - lattice_a;
            auto raw_surface_b = surface_mesh(lattice_b);
            auto upper_plate =
                Manifold::Cube(vec3(width * outer_scale, width * outer_scale,
                                    2.0 * plate + plate_overlap),
                               true)
                    .Translate(vec3(0.0, 0.0,
                                    half_height + plate - plate_overlap / 2.0));
            auto plate_union_b = lattice_b + upper_plate;
            auto final_crop_b =
                Manifold::Cube(vec3(width, width, 2.0 * half_height + plate), true)
                    .Translate(vec3(0.0, 0.0, plate / 2.0));
            auto boolean_b = plate_union_b ^ final_crop_b;
            if (config.part_b_construction ==
                domain::PartBConstruction::container_minus_part_a) {
                report(callbacks, 0.45,
                       "[Part B 构造] 使用完整目标包络减去 Part A，确保 A/B 外边界严格互补");
                const auto design_container =
                    Manifold::Cube(
                        vec3(width, width,
                             2.0 * half_height + 2.0 * plate),
                        true);
                boolean_b = design_container - boolean_a;
            } else {
                report(callbacks, 0.45,
                       "[Part B 构造] 使用共享隐式场的互补相，并独立并入上盖板");
            }
            auto plate_union_surface_b = surface_mesh(plate_union_b);
            auto boolean_surface_b = surface_mesh(boolean_b);
            auto part_b = make_part(boolean_b);
            part_b.raw_surface = std::move(raw_surface_b);
            part_b.plate_union_surface = std::move(plate_union_surface_b);
            part_b.boolean_surface = std::move(boolean_surface_b);
            remesh_part(part_b, config.mesh, callbacks, 0.58, "Part B");
            report(callbacks, 0.69, "Part B surface remeshing passed the two-manifold gate");

            const auto exact_total = width * width *
                                     (width * static_cast<double>(config.mesh.repeat_z) +
                                      2.0 * config.mesh.plate_thickness);
            const auto generated_total =
                part_a.surface_metrics.volume + part_b.surface_metrics.volume;
            if (std::abs(generated_total - exact_total) / exact_total > 0.05) {
                throw std::runtime_error{"A/B volume partition differs from the design domain by >5%"};
            }

            GeometryResult result{.part_a = std::move(part_a),
                                  .part_b = std::move(part_b),
                                  .phase_x = phase_x,
                                  .phase_y = phase_y,
                                  .grid_spacing = spacing,
                                  .surface_backend = "manifold-level-set",
                                  .tetrahedral_backend = {}};
            if (config.mesh.tetrahedralize) {
#ifdef MBS_ENABLE_TETGEN
                {
                    std::ostringstream settings_log;
                    settings_log << "[四面体化][参数] 类型="
                                 << (config.mesh.tetgen.order == 2 ? "Quadratic-Tet10 (C3D10)"
                                                                  : "Linear-Tet4 (C3D4)")
                                 << "，半径边长比≤" << config.mesh.tetgen.minimum_ratio
                                 << "，最小二面角=" << config.mesh.tetgen.minimum_dihedral
                                 << "°，优化级别=O" << config.mesh.tetgen.optimization_level
                                 << "，保持输入表面="
                                 << (config.mesh.tetgen.no_bisect ? "是" : "否")
                                 << "，目标边长=";
                    if (config.mesh.tetgen.target_edge_length_mm > 0.0) {
                        const auto maximum_volume =
                            std::pow(config.mesh.tetgen.target_edge_length_mm, 3.0) /
                            (6.0 * std::sqrt(2.0));
                        settings_log << config.mesh.tetgen.target_edge_length_mm
                                     << " mm（折算最大体积 -a" << maximum_volume << " mm³）";
                    } else {
                        settings_log << "自动";
                    }
                    report(callbacks, 0.70, settings_log.str());
                }
                report(callbacks, 0.70,
                       "[体网格准备] 保持当前相位，先执行 CGAL 精确自相交诊断与修复");
                prepare_for_volume_meshing(result.part_a, callbacks, 0.71, "Part A");
                prepare_for_volume_meshing(result.part_b, callbacks, 0.72, "Part B");
                report(callbacks, 0.72, "tetrahedralizing Part A with TetGen 1.6");
                result.part_a.volume = tetrahedralize(result.part_a.surface, config.mesh.tetgen);
                result.part_a.volume_metrics = tetrahedral_metrics(*result.part_a.volume);
                {
                    const auto& metrics = *result.part_a.volume_metrics;
                    std::ostringstream line;
                    line << "[四面体化][Part A][完成] 节点=" << metrics.points
                         << "，单元=" << metrics.tetrahedra
                         << "，最小有向体积=" << metrics.minimum_signed_volume
                         << " mm³，反转单元=" << metrics.non_positive_count;
                    report(callbacks, 0.83, line.str());
                }
                report(callbacks, 0.84, "tetrahedralizing Part B with TetGen 1.6");
                result.part_b.volume = tetrahedralize(result.part_b.surface, config.mesh.tetgen);
                result.part_b.volume_metrics = tetrahedral_metrics(*result.part_b.volume);
                {
                    const auto& metrics = *result.part_b.volume_metrics;
                    std::ostringstream line;
                    line << "[四面体化][Part B][完成] 节点=" << metrics.points
                         << "，单元=" << metrics.tetrahedra
                         << "，最小有向体积=" << metrics.minimum_signed_volume
                         << " mm³，反转单元=" << metrics.non_positive_count;
                    report(callbacks, 0.92, line.str());
                }
                result.tetrahedral_backend = "tetgen-1.6";
#else
                throw std::runtime_error{
                    "tetrahedralization requested, but MBS was built without TetGen"};
#endif
            }
            report(callbacks, 0.94, "geometry and mesh quality gates passed");
            return result;
        } catch (const std::exception& error) {
            last_error = error.what();
            if (!config.random_phase || attempt == attempts) {
                break;
            }
            report(callbacks, 0.02,
                   "[当前相位失败] " + last_error +
                       "；同相位修复路径已经用尽，按用户设置重新采样 rnd_x/rnd_y");
        }
    }
    throw std::runtime_error{"TPMS generation failed after " + std::to_string(attempts) +
                             " attempt(s): " + last_error};
}

PublishedGeometry publish_geometry(const GeometryResult& result,
                                   const domain::DesignConfig& config,
                                   const std::filesystem::path& output_directory) {
    if (output_directory.empty() || output_directory.filename().empty()) {
        throw std::invalid_argument{"geometry output directory must name a dedicated directory"};
    }
    const auto parent = output_directory.parent_path();
    if (parent.empty()) {
        throw std::invalid_argument{"geometry output directory must have a parent"};
    }
    std::filesystem::create_directories(parent);
    if (std::filesystem::exists(output_directory)) {
        throw std::runtime_error{"geometry output directory already exists: " +
                                 output_directory.string()};
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staging = parent / (output_directory.filename().string() + ".staging-" +
                                   std::to_string(stamp));
    std::filesystem::create_directories(staging / "visualization");
    try {
        write_obj(staging / "tpms-tri-A.obj", result.part_a.surface);
        write_obj(staging / "tpms-tri-B.obj", result.part_b.surface);
        write_surface_inp(staging / "tpms-tri-A.inp", result.part_a.surface, "PART-A");
        write_surface_inp(staging / "tpms-tri-B.inp", result.part_b.surface, "PART-B");
        const auto visualization = staging / "visualization";
        const auto sampling_width = config.mesh.width * 2.25;
        const auto sampling_height =
            config.mesh.width * static_cast<double>(config.mesh.repeat_z) +
            4.0 * config.mesh.plate_thickness;
        const auto sampling_spacing =
            config.mesh.width / static_cast<double>(config.mesh.resolution);
        write_sampling_grid_vti(visualization / "grid.vti", sampling_width, sampling_height,
                                sampling_spacing);
        write_surface_vtp(visualization / "prt_a.vtp", result.part_a.boolean_surface);
        write_surface_vtp(visualization / "prt_b.vtp", result.part_b.boolean_surface);
        write_surface_vtp(visualization / "prt_a_rms.vtp", result.part_a.surface);
        write_surface_vtp(visualization / "prt_b_rms.vtp", result.part_b.surface);
        write_surface_vtp(visualization / "prt_a0.vtp", result.part_a.raw_surface);
        write_surface_vtp(visualization / "prt_b0.vtp", result.part_b.raw_surface);
        write_surface_vtp(visualization / "prt_a1.vtp", result.part_a.plate_union_surface);
        write_surface_vtp(visualization / "prt_b1.vtp", result.part_b.plate_union_surface);
        const auto middle = box_surface(
            config.mesh.width * 1.5, config.mesh.width * 1.5,
            config.mesh.width * config.mesh.repeat_z + 2.0 * config.mesh.plate_thickness, 0.0);
        const auto crop_a = box_surface(
            config.mesh.width, config.mesh.width,
            config.mesh.width * config.mesh.repeat_z + config.mesh.plate_thickness,
            -config.mesh.plate_thickness / 2.0);
        const auto crop_b = box_surface(
            config.mesh.width, config.mesh.width,
            config.mesh.width * config.mesh.repeat_z + config.mesh.plate_thickness,
            config.mesh.plate_thickness / 2.0);
        const auto lower_plate = box_surface(
            config.mesh.width * 2.25, config.mesh.width * 2.25,
            2.0 * config.mesh.plate_thickness,
            -config.mesh.width * config.mesh.repeat_z / 2.0 - config.mesh.plate_thickness);
        const auto upper_plate = box_surface(
            config.mesh.width * 2.25, config.mesh.width * 2.25,
            2.0 * config.mesh.plate_thickness,
            config.mesh.width * config.mesh.repeat_z / 2.0 + config.mesh.plate_thickness);
        write_surface_vtp(visualization / "tl_a.vtp", crop_a);
        write_surface_vtp(visualization / "tl_b.vtp", crop_b);
        write_surface_vtp(visualization / "tl_c.vtp", middle);
        write_surface_vtp(visualization / "plt_a.vtp", lower_plate);
        write_surface_vtp(visualization / "plt_b.vtp", upper_plate);
        if (config.mesh.tetrahedralize) {
            write_tetrahedral_inp(staging / "tpms-tet-A.inp", *result.part_a.volume, "PART-A");
            write_tetrahedral_inp(staging / "tpms-tet-B.inp", *result.part_b.volume, "PART-B");
            write_tetrahedral_vtu(visualization / "prt_a_grid.vtu", *result.part_a.volume);
            write_tetrahedral_vtu(visualization / "prt_b_grid.vtu", *result.part_b.volume);
            write_tetrahedral_vtu(visualization / "prt_a_subgrid.vtu",
                                  clipped_tetrahedra(*result.part_a.volume, false));
            write_tetrahedral_vtu(visualization / "prt_b_subgrid.vtu",
                                  clipped_tetrahedra(*result.part_b.volume, false));
        }
        write_metadata(staging / "mesh_metadata.json", result, config);
        std::filesystem::rename(staging, output_directory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }

    PublishedGeometry published{.directory = output_directory, .files = {}};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(output_directory)) {
        if (entry.is_regular_file()) {
            published.files.push_back(entry.path());
        }
    }
    std::ranges::sort(published.files);
    return published;
}

std::string metrics_json(const GeometryResult& result) {
    std::ostringstream stream;
    stream << std::setprecision(15) << "{\"surface_backend\":\"" << result.surface_backend
           << "\",\"tetrahedral_backend\":\"" << result.tetrahedral_backend
           << "\",\"grid_spacing\":" << result.grid_spacing << ",\"phase_x\":"
           << result.phase_x << ",\"phase_y\":" << result.phase_y << ",\"part_a\":"
           << part_json(result.part_a) << ",\"part_b\":" << part_json(result.part_b) << '}';
    return stream.str();
}

} // namespace mbs::geometry
