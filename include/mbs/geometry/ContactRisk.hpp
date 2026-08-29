#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mbs::geometry {

struct PartRiskSummary final {
    std::string part;
    std::size_t element_count{};
    std::size_t contact_adjacent_count{};
    double minimum_volume{};
    double minimum_dihedral{};
    double maximum_radius_edge_ratio{};
    double minimum_height{};
    double minimum_normalized_volume{};
    std::size_t poor_dihedral_count{};
    std::size_t poor_normalized_volume_count{};
    std::size_t worst_dihedral_element{};
    std::size_t worst_normalized_element{};
};

struct ContactSummary final {
    std::size_t part_b_contact_node_count{};
    std::size_t penetrating_node_count{};
    double maximum_penetration{};
    double mean_positive_penetration{};
    std::size_t worst_node_label{};
};

struct AdjustmentSummary final {
    std::size_t affected_element_count{};
    std::size_t inverted_element_count{};
    std::size_t small_volume_element_count{};
    std::size_t volume_below_ten_percent_count{};
    double minimum_post_adjustment_volume_ratio{1.0};
    std::vector<std::size_t> sample_inverted_element_labels;
};

struct ContactRiskReport final {
    std::string risk_level;
    double boundary_tolerance{};
    double small_volume_threshold{1.0e-9};
    PartRiskSummary part_a;
    PartRiskSummary part_b;
    ContactSummary contact;
    AdjustmentSummary adjustment;

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string format() const;
};

[[nodiscard]] ContactRiskReport analyze_contact_risk(
    const std::filesystem::path& mesh_directory, double boundary_tolerance = 0.01,
    double small_volume = 1.0e-9);

void save_contact_risk(const ContactRiskReport& report, const std::filesystem::path& path);

} // namespace mbs::geometry
