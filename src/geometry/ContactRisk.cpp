#include "mbs/geometry/ContactRisk.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace mbs::geometry {
namespace {

using Vec3 = std::array<double, 3>;
using Tet = std::array<std::size_t, 4>;

constexpr std::array<std::array<int, 4>, 4> faces{{{0, 1, 2, 3}, {0, 3, 1, 2},
                                                   {1, 3, 2, 0}, {2, 3, 0, 1}}};

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 multiply(const Vec3& a, const double value) {
    return {a[0] * value, a[1] * value, a[2] * value};
}
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
double norm_squared(const Vec3& value) { return dot(value, value); }
double norm(const Vec3& value) { return std::sqrt(norm_squared(value)); }
Vec3 normalize(const Vec3& value) {
    const auto length = norm(value);
    return length > 0.0 ? multiply(value, 1.0 / length) : Vec3{};
}

struct TetMesh final {
    std::vector<std::size_t> node_labels;
    std::vector<Vec3> points;
    std::vector<std::size_t> element_labels;
    std::vector<Tet> cells;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> csv_values(const std::string& line) {
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto comma = line.find(',', begin);
        values.push_back(trim(line.substr(begin, comma == std::string::npos
                                                    ? std::string::npos
                                                    : comma - begin)));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return values;
}

TetMesh read_c3d4(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot open C3D4/C3D10 mesh: " + path.string()};
    }
    enum class Section { none, nodes, elements } section = Section::none;
    std::map<std::size_t, Vec3> nodes;
    std::map<std::size_t, std::array<std::size_t, 4>> elements;
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.starts_with("**")) {
            continue;
        }
        if (line.front() == '*') {
            auto upper = line;
            std::ranges::transform(upper, upper.begin(), [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
            section = upper.starts_with("*NODE")
                          ? Section::nodes
                          : upper.starts_with("*ELEMENT") &&
                                    (upper.find("C3D4") != std::string::npos ||
                                     upper.find("C3D10") != std::string::npos)
                                ? Section::elements
                                : Section::none;
            continue;
        }
        const auto values = csv_values(line);
        try {
            if (section == Section::nodes && values.size() >= 4) {
                nodes[std::stoull(values[0])] =
                    {std::stod(values[1]), std::stod(values[2]), std::stod(values[3])};
            } else if (section == Section::elements && values.size() >= 5) {
                elements[std::stoull(values[0])] = {std::stoull(values[1]),
                                                     std::stoull(values[2]),
                                                     std::stoull(values[3]),
                                                     std::stoull(values[4])};
            }
        } catch (const std::exception&) {
            throw std::runtime_error{"invalid C3D4/C3D10 record in " + path.string()};
        }
    }
    if (nodes.empty() || elements.empty()) {
        throw std::runtime_error{"no C3D4/C3D10 nodes/elements found in " + path.string()};
    }
    TetMesh result;
    std::unordered_map<std::size_t, std::size_t> node_index;
    result.node_labels.reserve(nodes.size());
    result.points.reserve(nodes.size());
    for (const auto& [label, point] : nodes) {
        node_index.emplace(label, result.points.size());
        result.node_labels.push_back(label);
        result.points.push_back(point);
    }
    result.element_labels.reserve(elements.size());
    result.cells.reserve(elements.size());
    for (const auto& [label, node_labels] : elements) {
        Tet cell{};
        for (std::size_t index = 0; index < cell.size(); ++index) {
            const auto found = node_index.find(node_labels[index]);
            if (found == node_index.end()) {
                throw std::runtime_error{"tetrahedral element references an unknown node"};
            }
            cell[index] = found->second;
        }
        result.element_labels.push_back(label);
        result.cells.push_back(cell);
    }
    return result;
}

struct Triangle final {
    std::array<std::size_t, 3> vertices{};
    std::size_t adjacent{};
    Vec3 normal{};
};

std::vector<Triangle> contact_surface(const TetMesh& mesh, const double tolerance) {
    struct Owner final {
        std::size_t cell{};
        std::array<std::size_t, 3> face{};
        std::size_t opposite{};
        int count{};
    };
    std::map<std::array<std::size_t, 3>, Owner> owners;
    for (std::size_t cell_index = 0; cell_index < mesh.cells.size(); ++cell_index) {
        const auto& cell = mesh.cells[cell_index];
        for (const auto& local : faces) {
            std::array<std::size_t, 3> face{
                cell[static_cast<std::size_t>(local[0])],
                cell[static_cast<std::size_t>(local[1])],
                cell[static_cast<std::size_t>(local[2])]};
            auto key = face;
            std::ranges::sort(key);
            auto [position, inserted] = owners.try_emplace(
                key, Owner{.cell = cell_index,
                           .face = face,
                           .opposite = cell[static_cast<std::size_t>(local[3])],
                           .count = 1});
            if (!inserted) {
                ++position->second.count;
            }
        }
    }
    Vec3 minimum = mesh.points.front();
    Vec3 maximum = mesh.points.front();
    for (const auto& point : mesh.points) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], point[axis]);
            maximum[axis] = std::max(maximum[axis], point[axis]);
        }
    }
    std::vector<Triangle> triangles;
    for (const auto& [unused, owner] : owners) {
        static_cast<void>(unused);
        if (owner.count != 1) {
            continue;
        }
        bool on_box = false;
        for (std::size_t axis = 0; axis < 3 && !on_box; ++axis) {
            const auto all_min = std::ranges::all_of(owner.face, [&](const auto vertex) {
                return std::abs(mesh.points[vertex][axis] - minimum[axis]) <= tolerance;
            });
            const auto all_max = std::ranges::all_of(owner.face, [&](const auto vertex) {
                return std::abs(mesh.points[vertex][axis] - maximum[axis]) <= tolerance;
            });
            on_box = all_min || all_max;
        }
        if (on_box) {
            continue;
        }
        auto face = owner.face;
        auto normal = cross(subtract(mesh.points[face[1]], mesh.points[face[0]]),
                            subtract(mesh.points[face[2]], mesh.points[face[0]]));
        if (dot(normal, subtract(mesh.points[owner.opposite], mesh.points[face[0]])) > 0.0) {
            std::swap(face[1], face[2]);
            normal = multiply(normal, -1.0);
        }
        triangles.push_back({.vertices = face,
                             .adjacent = owner.cell,
                             .normal = normalize(normal)});
    }
    if (triangles.empty()) {
        throw std::runtime_error{"no contact faces were identified"};
    }
    return triangles;
}

struct TetMetric final {
    double signed_volume{};
    double volume{};
    double minimum_dihedral{};
    double radius_edge_ratio{};
    double minimum_height{};
    double normalized_volume{};
};

double determinant(const std::array<Vec3, 3>& matrix) {
    return matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
           matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
           matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
}

std::vector<TetMetric> tet_metrics(const std::vector<Vec3>& points,
                                   const std::vector<Tet>& cells) {
    std::vector<TetMetric> result;
    result.reserve(cells.size());
    constexpr std::array<std::array<int, 2>, 6> edge_pairs{
        {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
    for (const auto& cell : cells) {
        std::array<Vec3, 4> xyz{};
        for (std::size_t index = 0; index < xyz.size(); ++index) {
            xyz[index] = points[cell[index]];
        }
        const auto signed_volume =
            dot(cross(subtract(xyz[1], xyz[0]), subtract(xyz[2], xyz[0])),
                subtract(xyz[3], xyz[0])) /
            6.0;
        const auto volume = std::abs(signed_volume);
        std::array<double, 6> edges{};
        for (std::size_t index = 0; index < edge_pairs.size(); ++index) {
            edges[index] = norm(subtract(
                xyz[static_cast<std::size_t>(edge_pairs[index][0])],
                xyz[static_cast<std::size_t>(edge_pairs[index][1])]));
        }
        const auto edge_squared_sum =
            std::accumulate(edges.begin(), edges.end(), 0.0,
                            [](const double sum, const double edge) { return sum + edge * edge; });
        const auto rms_edge = std::sqrt(edge_squared_sum / 6.0);
        const auto normalized_volume =
            6.0 * std::sqrt(2.0) * volume /
            std::max(rms_edge * rms_edge * rms_edge, std::numeric_limits<double>::min());

        std::array<double, 4> areas{};
        std::array<Vec3, 4> normals{};
        for (std::size_t index = 0; index < faces.size(); ++index) {
            const auto& local = faces[index];
            auto normal = cross(
                subtract(xyz[static_cast<std::size_t>(local[1])],
                         xyz[static_cast<std::size_t>(local[0])]),
                subtract(xyz[static_cast<std::size_t>(local[2])],
                         xyz[static_cast<std::size_t>(local[0])]));
            if (dot(normal, subtract(xyz[static_cast<std::size_t>(local[3])],
                                     xyz[static_cast<std::size_t>(local[0])])) > 0.0) {
                normal = multiply(normal, -1.0);
            }
            const auto length = norm(normal);
            areas[index] = length * 0.5;
            normals[index] = length > 0.0 ? multiply(normal, 1.0 / length) : Vec3{};
        }
        auto minimum_dihedral = 180.0;
        for (std::size_t first = 0; first < normals.size(); ++first) {
            for (std::size_t second = first + 1; second < normals.size(); ++second) {
                const auto cosine = std::clamp(dot(normals[first], normals[second]), -1.0, 1.0);
                const auto angle =
                    180.0 - std::acos(cosine) * 180.0 / std::numbers::pi;
                minimum_dihedral = std::min(minimum_dihedral, angle);
            }
        }
        const auto maximum_area = *std::ranges::max_element(areas);
        const auto minimum_height =
            3.0 * volume / std::max(maximum_area, std::numeric_limits<double>::min());

        std::array<Vec3, 3> matrix{};
        Vec3 rhs{};
        for (std::size_t row = 0; row < 3; ++row) {
            matrix[row] = multiply(subtract(xyz[row + 1], xyz[0]), 2.0);
            rhs[row] = norm_squared(xyz[row + 1]) - norm_squared(xyz[0]);
        }
        auto radius_edge_ratio = std::numeric_limits<double>::infinity();
        const auto denominator = determinant(matrix);
        if (std::abs(denominator) > std::numeric_limits<double>::min()) {
            Vec3 center{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                auto replaced = matrix;
                for (std::size_t row = 0; row < 3; ++row) {
                    replaced[row][axis] = rhs[row];
                }
                center[axis] = determinant(replaced) / denominator;
            }
            radius_edge_ratio = norm(subtract(center, xyz[0])) /
                                std::max(*std::ranges::min_element(edges),
                                         std::numeric_limits<double>::min());
        }
        result.push_back({.signed_volume = signed_volume,
                          .volume = volume,
                          .minimum_dihedral = minimum_dihedral,
                          .radius_edge_ratio = radius_edge_ratio,
                          .minimum_height = minimum_height,
                          .normalized_volume = normalized_volume});
    }
    return result;
}

PartRiskSummary summarize(const std::string& name, const TetMesh& mesh,
                          const std::vector<Triangle>& contact) {
    const auto metrics = tet_metrics(mesh.points, mesh.cells);
    std::set<std::size_t> adjacent;
    for (const auto& triangle : contact) {
        adjacent.insert(triangle.adjacent);
    }
    PartRiskSummary summary{.part = name,
                            .element_count = mesh.cells.size(),
                            .contact_adjacent_count = adjacent.size(),
                            .minimum_volume = std::numeric_limits<double>::infinity(),
                            .minimum_dihedral = std::numeric_limits<double>::infinity(),
                            .maximum_radius_edge_ratio = 0.0,
                            .minimum_height = std::numeric_limits<double>::infinity(),
                            .minimum_normalized_volume = std::numeric_limits<double>::infinity()};
    for (const auto& metric : metrics) {
        summary.minimum_volume = std::min(summary.minimum_volume, metric.volume);
    }
    for (const auto index : adjacent) {
        const auto& metric = metrics[index];
        if (metric.minimum_dihedral < summary.minimum_dihedral) {
            summary.minimum_dihedral = metric.minimum_dihedral;
            summary.worst_dihedral_element = mesh.element_labels[index];
        }
        if (metric.normalized_volume < summary.minimum_normalized_volume) {
            summary.minimum_normalized_volume = metric.normalized_volume;
            summary.worst_normalized_element = mesh.element_labels[index];
        }
        summary.maximum_radius_edge_ratio =
            std::max(summary.maximum_radius_edge_ratio, metric.radius_edge_ratio);
        summary.minimum_height = std::min(summary.minimum_height, metric.minimum_height);
        summary.poor_dihedral_count += metric.minimum_dihedral < 5.0 ? 1U : 0U;
        summary.poor_normalized_volume_count += metric.normalized_volume < 0.02 ? 1U : 0U;
    }
    return summary;
}

Vec3 closest_point_on_triangle(const Vec3& point, const Vec3& a, const Vec3& b, const Vec3& c) {
    const auto ab = subtract(b, a);
    const auto ac = subtract(c, a);
    const auto ap = subtract(point, a);
    const auto d1 = dot(ab, ap);
    const auto d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return a;
    }
    const auto bp = subtract(point, b);
    const auto d3 = dot(ab, bp);
    const auto d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return b;
    }
    const auto vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return add(a, multiply(ab, d1 / (d1 - d3)));
    }
    const auto cp = subtract(point, c);
    const auto d5 = dot(ab, cp);
    const auto d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return c;
    }
    const auto vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return add(a, multiply(ac, d2 / (d2 - d6)));
    }
    const auto va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
        return add(b, multiply(subtract(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }
    const auto denominator = 1.0 / (va + vb + vc);
    return add(a, add(multiply(ab, vb * denominator), multiply(ac, vc * denominator)));
}

struct Aabb final {
    Vec3 minimum{std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity()};
    Vec3 maximum{-std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity()};
};

double box_distance_squared(const Aabb& box, const Vec3& point) {
    double distance = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto delta = point[axis] < box.minimum[axis]
                               ? box.minimum[axis] - point[axis]
                               : point[axis] > box.maximum[axis] ? point[axis] - box.maximum[axis]
                                                                  : 0.0;
        distance += delta * delta;
    }
    return distance;
}

class TriangleBvh final {
  public:
    TriangleBvh(const std::vector<Vec3>& points, const std::vector<Triangle>& triangles)
        : points_(points), triangles_(triangles), indices_(triangles.size()) {
        std::iota(indices_.begin(), indices_.end(), std::size_t{});
        root_ = build(0, indices_.size());
    }

    struct Hit final {
        std::size_t triangle{std::numeric_limits<std::size_t>::max()};
        Vec3 point{};
        double distance_squared{std::numeric_limits<double>::infinity()};
    };

    [[nodiscard]] Hit closest(const Vec3& point) const {
        Hit hit;
        search(root_, point, hit);
        return hit;
    }

  private:
    struct Node final {
        Aabb box;
        std::size_t begin{};
        std::size_t end{};
        std::optional<std::size_t> left;
        std::optional<std::size_t> right;
    };

    const std::vector<Vec3>& points_;
    const std::vector<Triangle>& triangles_;
    std::vector<std::size_t> indices_;
    std::vector<Node> nodes_;
    std::size_t root_{};

    Aabb bounds(const std::size_t begin, const std::size_t end) const {
        Aabb result;
        for (std::size_t position = begin; position < end; ++position) {
            const auto& triangle = triangles_[indices_[position]];
            for (const auto vertex : triangle.vertices) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    result.minimum[axis] = std::min(result.minimum[axis], points_[vertex][axis]);
                    result.maximum[axis] = std::max(result.maximum[axis], points_[vertex][axis]);
                }
            }
        }
        return result;
    }

    std::size_t build(const std::size_t begin, const std::size_t end) {
        const auto node_index = nodes_.size();
        nodes_.push_back(
            {.box = bounds(begin, end), .begin = begin, .end = end, .left = {}, .right = {}});
        if (end - begin <= 8) {
            return node_index;
        }
        const auto& box = nodes_[node_index].box;
        std::size_t axis = 0;
        for (std::size_t candidate = 1; candidate < 3; ++candidate) {
            if (box.maximum[candidate] - box.minimum[candidate] >
                box.maximum[axis] - box.minimum[axis]) {
                axis = candidate;
            }
        }
        const auto middle = begin + (end - begin) / 2;
        std::nth_element(indices_.begin() + static_cast<std::ptrdiff_t>(begin),
                         indices_.begin() + static_cast<std::ptrdiff_t>(middle),
                         indices_.begin() + static_cast<std::ptrdiff_t>(end),
                         [&](const auto first, const auto second) {
                             const auto centroid = [&](const std::size_t triangle_index) {
                                 const auto& triangle = triangles_[triangle_index];
                                 return (points_[triangle.vertices[0]][axis] +
                                         points_[triangle.vertices[1]][axis] +
                                         points_[triangle.vertices[2]][axis]) /
                                        3.0;
                             };
                             return centroid(first) < centroid(second);
                         });
        const auto left = build(begin, middle);
        const auto right = build(middle, end);
        nodes_[node_index].left = left;
        nodes_[node_index].right = right;
        return node_index;
    }

    void search(const std::size_t node_index, const Vec3& point, Hit& hit) const {
        const auto& node = nodes_[node_index];
        constexpr double tie_tolerance = 1.0e-20;
        if (box_distance_squared(node.box, point) > hit.distance_squared + tie_tolerance) {
            return;
        }
        if (!node.left) {
            for (std::size_t position = node.begin; position < node.end; ++position) {
                const auto triangle_index = indices_[position];
                const auto& triangle = triangles_[triangle_index];
                const auto closest = closest_point_on_triangle(
                    point, points_[triangle.vertices[0]], points_[triangle.vertices[1]],
                    points_[triangle.vertices[2]]);
                const auto distance = norm_squared(subtract(point, closest));
                if (distance + tie_tolerance < hit.distance_squared ||
                    (std::abs(distance - hit.distance_squared) <= tie_tolerance &&
                     triangle_index < hit.triangle)) {
                    hit = {.triangle = triangle_index,
                           .point = closest,
                           .distance_squared = distance};
                }
            }
            return;
        }
        const auto left_distance = box_distance_squared(nodes_[*node.left].box, point);
        const auto right_distance = box_distance_squared(nodes_[*node.right].box, point);
        if (left_distance <= right_distance) {
            search(*node.left, point, hit);
            search(*node.right, point, hit);
        } else {
            search(*node.right, point, hit);
            search(*node.left, point, hit);
        }
    }
};

std::string part_json(const PartRiskSummary& part) {
    std::ostringstream stream;
    stream << std::setprecision(15) << "{\"part\":\"" << part.part
           << "\",\"element_count\":" << part.element_count
           << ",\"contact_adjacent_count\":" << part.contact_adjacent_count
           << ",\"minimum_volume_mm3\":" << part.minimum_volume
           << ",\"minimum_dihedral_deg\":" << part.minimum_dihedral
           << ",\"maximum_radius_edge_ratio\":" << part.maximum_radius_edge_ratio
           << ",\"minimum_height_mm\":" << part.minimum_height
           << ",\"minimum_normalized_volume\":" << part.minimum_normalized_volume
           << ",\"poor_dihedral_count\":" << part.poor_dihedral_count
           << ",\"poor_normalized_volume_count\":" << part.poor_normalized_volume_count
           << ",\"worst_dihedral_element\":" << part.worst_dihedral_element
           << ",\"worst_normalized_element\":" << part.worst_normalized_element << '}';
    return stream.str();
}

} // namespace

ContactRiskReport analyze_contact_risk(const std::filesystem::path& mesh_directory,
                                       const double boundary_tolerance,
                                       const double small_volume) {
    const auto part_a_mesh = read_c3d4(mesh_directory / "tpms-tet-A.inp");
    const auto part_b_mesh = read_c3d4(mesh_directory / "tpms-tet-B.inp");
    const auto part_a_surface = contact_surface(part_a_mesh, boundary_tolerance);
    const auto part_b_surface = contact_surface(part_b_mesh, boundary_tolerance);
    ContactRiskReport report;
    report.boundary_tolerance = boundary_tolerance;
    report.small_volume_threshold = small_volume;
    report.part_a = summarize("A", part_a_mesh, part_a_surface);
    report.part_b = summarize("B", part_b_mesh, part_b_surface);

    std::set<std::size_t> contact_nodes_set;
    for (const auto& triangle : part_b_surface) {
        contact_nodes_set.insert(triangle.vertices.begin(), triangle.vertices.end());
    }
    const std::vector<std::size_t> contact_nodes(contact_nodes_set.begin(), contact_nodes_set.end());
    TriangleBvh locator(part_a_mesh.points, part_a_surface);
    auto moved_points = part_b_mesh.points;
    std::vector<bool> moved_node(part_b_mesh.points.size());
    double penetration_sum = 0.0;
    std::size_t worst_index = 0;
    for (std::size_t index = 0; index < contact_nodes.size(); ++index) {
        const auto node = contact_nodes[index];
        const auto hit = locator.closest(part_b_mesh.points[node]);
        const auto signed_gap = dot(subtract(part_b_mesh.points[node], hit.point),
                                    part_a_surface[hit.triangle].normal);
        const auto penetration = std::max(-signed_gap, 0.0);
        if (penetration > 0.0) {
            moved_node[node] = true;
            moved_points[node] = hit.point;
            penetration_sum += penetration;
            ++report.contact.penetrating_node_count;
        }
        if (penetration > report.contact.maximum_penetration) {
            report.contact.maximum_penetration = penetration;
            worst_index = index;
        }
    }
    report.contact.part_b_contact_node_count = contact_nodes.size();
    report.contact.mean_positive_penetration =
        report.contact.penetrating_node_count == 0
            ? 0.0
            : penetration_sum / static_cast<double>(report.contact.penetrating_node_count);
    if (!contact_nodes.empty()) {
        report.contact.worst_node_label = part_b_mesh.node_labels[contact_nodes[worst_index]];
    }

    const auto original = tet_metrics(part_b_mesh.points, part_b_mesh.cells);
    const auto adjusted = tet_metrics(moved_points, part_b_mesh.cells);
    report.adjustment.minimum_post_adjustment_volume_ratio = 1.0;
    bool has_affected = false;
    for (std::size_t element = 0; element < part_b_mesh.cells.size(); ++element) {
        const auto& cell = part_b_mesh.cells[element];
        const bool affected = moved_node[cell[0]] || moved_node[cell[1]] || moved_node[cell[2]] ||
                              moved_node[cell[3]];
        if (!affected) {
            continue;
        }
        has_affected = true;
        ++report.adjustment.affected_element_count;
        const auto inverted = adjusted[element].signed_volume == 0.0 ||
                              std::signbit(adjusted[element].signed_volume) !=
                                  std::signbit(original[element].signed_volume);
        const auto small = adjusted[element].volume <= small_volume;
        const auto ratio = adjusted[element].volume /
                           std::max(original[element].volume,
                                    std::numeric_limits<double>::min());
        if (inverted) {
            ++report.adjustment.inverted_element_count;
            if (report.adjustment.sample_inverted_element_labels.size() < 20) {
                report.adjustment.sample_inverted_element_labels.push_back(
                    part_b_mesh.element_labels[element]);
            }
        }
        report.adjustment.small_volume_element_count += small ? 1U : 0U;
        report.adjustment.volume_below_ten_percent_count += ratio < 0.1 ? 1U : 0U;
        report.adjustment.minimum_post_adjustment_volume_ratio =
            std::min(report.adjustment.minimum_post_adjustment_volume_ratio, ratio);
    }
    if (!has_affected) {
        report.adjustment.minimum_post_adjustment_volume_ratio = 1.0;
    }
    const auto quality_bad = report.part_a.poor_dihedral_count != 0 ||
                             report.part_a.poor_normalized_volume_count != 0 ||
                             report.part_b.poor_dihedral_count != 0 ||
                             report.part_b.poor_normalized_volume_count != 0;
    report.risk_level = report.adjustment.inverted_element_count != 0 ||
                                report.adjustment.small_volume_element_count != 0 ||
                                report.contact.maximum_penetration > 0.05
                            ? "HIGH"
                        : quality_bad || report.adjustment.volume_below_ten_percent_count != 0
                            ? "MEDIUM"
                            : "LOW";
    return report;
}

std::string ContactRiskReport::to_json() const {
    std::ostringstream stream;
    stream << std::setprecision(15)
           << "{\"schema_version\":1,\"method\":\"Conservative closest-point projection "
              "of penetrating Part B contact nodes onto Part A\",\"risk_level\":\""
           << risk_level << "\",\"thresholds\":{\"minimum_dihedral_deg\":5,"
              "\"minimum_normalized_volume\":0.02,\"critical_volume_ratio\":0.1,"
              "\"boundary_tolerance_mm\":"
           << boundary_tolerance << ",\"small_volume_mm3\":" << small_volume_threshold
           << "},\"part_a\":"
           << part_json(part_a) << ",\"part_b\":" << part_json(part_b)
           << ",\"contact\":{\"part_b_contact_node_count\":"
           << contact.part_b_contact_node_count << ",\"penetrating_node_count\":"
           << contact.penetrating_node_count << ",\"maximum_penetration_mm\":"
           << contact.maximum_penetration << ",\"mean_positive_penetration_mm\":"
           << contact.mean_positive_penetration << ",\"worst_node_label\":"
           << contact.worst_node_label << "},\"adjustment_simulation\":{"
              "\"affected_element_count\":"
           << adjustment.affected_element_count << ",\"inverted_element_count\":"
           << adjustment.inverted_element_count << ",\"small_volume_element_count\":"
           << adjustment.small_volume_element_count
           << ",\"volume_below_10_percent_count\":"
           << adjustment.volume_below_ten_percent_count
           << ",\"minimum_post_adjustment_volume_ratio\":"
           << adjustment.minimum_post_adjustment_volume_ratio
           << ",\"sample_inverted_element_labels\":[";
    for (std::size_t index = 0; index < adjustment.sample_inverted_element_labels.size(); ++index) {
        stream << adjustment.sample_inverted_element_labels[index];
        if (index + 1 != adjustment.sample_inverted_element_labels.size()) {
            stream << ',';
        }
    }
    stream << "]}}";
    return stream.str();
}

std::string ContactRiskReport::format() const {
    std::ostringstream stream;
    stream << std::setprecision(6) << "=== Contact adjustment risk check ===\nRisk level: "
           << risk_level << '\n';
    for (const auto* part : {&part_a, &part_b}) {
        stream << "Part " << part->part << ": contact tets=" << part->contact_adjacent_count
               << ", min dihedral=" << part->minimum_dihedral << " deg (element "
               << part->worst_dihedral_element << "), max R/lmin="
               << part->maximum_radius_edge_ratio << ", min height=" << part->minimum_height
               << " mm, min normalized volume=" << part->minimum_normalized_volume
               << " (element " << part->worst_normalized_element << ")\n";
    }
    stream << "B->A contact: nodes=" << contact.part_b_contact_node_count
           << ", penetrating=" << contact.penetrating_node_count
           << ", max penetration=" << contact.maximum_penetration << " mm (B node "
           << contact.worst_node_label << "), mean penetration="
           << contact.mean_positive_penetration << " mm\nProjection simulation: affected tets="
           << adjustment.affected_element_count << ", inverted="
           << adjustment.inverted_element_count << ", small="
           << adjustment.small_volume_element_count << ", volume<10%="
           << adjustment.volume_below_ten_percent_count << ", min volume ratio="
           << adjustment.minimum_post_adjustment_volume_ratio
           << "\nNote: projection is a conservative screening estimate, not an exact Abaqus contact "
              "adjustment replay.";
    return stream.str();
}

void save_contact_risk(const ContactRiskReport& report, const std::filesystem::path& path) {
    std::ofstream stream{path};
    if (!stream) {
        throw std::runtime_error{"cannot write contact-risk report: " + path.string()};
    }
    stream << report.to_json() << '\n';
}

} // namespace mbs::geometry
