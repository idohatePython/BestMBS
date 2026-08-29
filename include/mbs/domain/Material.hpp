#pragma once

#include "mbs/domain/Validation.hpp"

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::domain {

inline constexpr int material_schema_version = 1;

enum class HyperelasticModel { mooney_rivlin, neo_hooke, yeoh, ogden };

struct DensityBehavior final {
    bool enabled{false};
    double value{};
};

struct ElasticBehavior final {
    bool enabled{true};
    double youngs_modulus{};
    double poissons_ratio{};
};

struct PlasticPoint final {
    double yield_stress{};
    double plastic_strain{};
};

struct PlasticBehavior final {
    bool enabled{false};
    std::vector<PlasticPoint> table;
};

struct HyperelasticBehavior final {
    bool enabled{false};
    HyperelasticModel model{HyperelasticModel::mooney_rivlin};
    int order{1};
    std::map<std::string, double, std::less<>> coefficients;
};

struct MaterialDefinition final {
    std::string name;
    DensityBehavior density{};
    ElasticBehavior elastic{};
    PlasticBehavior plastic{};
    HyperelasticBehavior hyperelastic{};

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

struct MaterialLibrary final {
    int schema_version{material_schema_version};
    std::vector<MaterialDefinition> materials;

    [[nodiscard]] ValidationErrors validation_errors() const;
    [[nodiscard]] bool is_valid() const;
};

[[nodiscard]] std::vector<std::string> coefficient_names(HyperelasticModel model, int order = 1);
[[nodiscard]] std::string_view to_string(HyperelasticModel model) noexcept;
[[nodiscard]] std::vector<MaterialDefinition> builtin_materials();
[[nodiscard]] std::array<MaterialDefinition, 2> default_material_pair();

} // namespace mbs::domain
