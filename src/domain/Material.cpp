#include "mbs/domain/Material.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace mbs::domain {
namespace {

bool blank(const std::string_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](const unsigned char character) { return std::isspace(character) != 0; });
}

std::string case_fold_ascii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void require_finite(ValidationErrors& errors, const double value, const char* message) {
    if (!std::isfinite(value)) {
        errors.emplace_back(message);
    }
}

} // namespace

ValidationErrors MaterialDefinition::validation_errors() const {
    ValidationErrors errors;
    if (name.empty() || blank(name)) {
        errors.emplace_back("material name must not be blank");
    }
    if (density.enabled && (!std::isfinite(density.value) || density.value <= 0.0)) {
        errors.emplace_back("density must be finite and greater than 0");
    }
    if (elastic.enabled) {
        if (!std::isfinite(elastic.youngs_modulus) || elastic.youngs_modulus <= 0.0) {
            errors.emplace_back("Young's modulus must be finite and greater than 0");
        }
        if (!std::isfinite(elastic.poissons_ratio) || elastic.poissons_ratio <= -1.0 ||
            elastic.poissons_ratio >= 0.5) {
            errors.emplace_back("Poisson's ratio must be in (-1, 0.5)");
        }
    }
    if (plastic.enabled) {
        if (!elastic.enabled) {
            errors.emplace_back("plastic behavior requires linear elasticity");
        }
        if (plastic.table.empty()) {
            errors.emplace_back("plastic table must not be empty when plasticity is enabled");
        }
        double previous_strain = -1.0;
        for (std::size_t index = 0; index < plastic.table.size(); ++index) {
            const auto& point = plastic.table[index];
            if (!std::isfinite(point.yield_stress) || point.yield_stress <= 0.0 ||
                !std::isfinite(point.plastic_strain) || point.plastic_strain < 0.0) {
                errors.emplace_back(
                    "plastic points require positive stress and non-negative strain");
                break;
            }
            if (index == 0 && std::abs(point.plastic_strain) > 1.0e-12) {
                errors.emplace_back("the first plastic strain must be 0");
            }
            if (point.plastic_strain < previous_strain) {
                errors.emplace_back("plastic strain must be non-decreasing");
                break;
            }
            previous_strain = point.plastic_strain;
        }
    }
    if (hyperelastic.enabled) {
        if (elastic.enabled || plastic.enabled) {
            errors.emplace_back("hyperelasticity cannot be combined with elasticity or plasticity");
        }
        if (hyperelastic.model == HyperelasticModel::ogden &&
            (hyperelastic.order < 1 || hyperelastic.order > 6)) {
            errors.emplace_back("Ogden order must be in [1, 6]");
        }
        const auto expected = coefficient_names(hyperelastic.model, hyperelastic.order);
        for (const auto& coefficient_name : expected) {
            const auto iterator = hyperelastic.coefficients.find(coefficient_name);
            if (iterator == hyperelastic.coefficients.end()) {
                errors.emplace_back("missing hyperelastic coefficient: " + coefficient_name);
                continue;
            }
            require_finite(errors, iterator->second, "hyperelastic coefficients must be finite");
            if (coefficient_name.starts_with('D') && iterator->second < 0.0) {
                errors.emplace_back(coefficient_name + " must not be negative");
            }
        }
    }
    if (!elastic.enabled && !hyperelastic.enabled) {
        errors.emplace_back("material must enable elasticity or hyperelasticity");
    }
    return errors;
}

bool MaterialDefinition::is_valid() const { return validation_errors().empty(); }

ValidationErrors MaterialLibrary::validation_errors() const {
    ValidationErrors errors;
    if (schema_version != material_schema_version) {
        errors.emplace_back("unsupported material schema version");
    }
    std::set<std::string, std::less<>> names;
    for (const auto& material : materials) {
        auto material_errors = material.validation_errors();
        errors.insert(errors.end(), material_errors.begin(), material_errors.end());
        if (!names.insert(case_fold_ascii(material.name)).second) {
            errors.emplace_back("material names must be unique ignoring ASCII case");
        }
    }
    return errors;
}

bool MaterialLibrary::is_valid() const { return validation_errors().empty(); }

std::vector<std::string> coefficient_names(const HyperelasticModel model, const int order) {
    switch (model) {
    case HyperelasticModel::mooney_rivlin:
        return {"C10", "C01", "D1"};
    case HyperelasticModel::neo_hooke:
        return {"C10", "D1"};
    case HyperelasticModel::yeoh:
        return {"C10", "C20", "C30", "D1", "D2", "D3"};
    case HyperelasticModel::ogden: {
        std::vector<std::string> names;
        if (order < 1) {
            return names;
        }
        names.reserve(static_cast<std::size_t>(order * 3));
        for (int index = 1; index <= order; ++index) {
            names.emplace_back("mu" + std::to_string(index));
            names.emplace_back("alpha" + std::to_string(index));
        }
        for (int index = 1; index <= order; ++index) {
            names.emplace_back("D" + std::to_string(index));
        }
        return names;
    }
    }
    return {};
}

std::string_view to_string(const HyperelasticModel model) noexcept {
    switch (model) {
    case HyperelasticModel::mooney_rivlin:
        return "mooney_rivlin";
    case HyperelasticModel::neo_hooke:
        return "neo_hooke";
    case HyperelasticModel::yeoh:
        return "yeoh";
    case HyperelasticModel::ogden:
        return "ogden";
    }
    return "unknown";
}

std::vector<MaterialDefinition> builtin_materials() {
    return {
        MaterialDefinition{
            .name = "Bambu PLA-CF",
            .density = {},
            .elastic = {.enabled = true, .youngs_modulus = 3949.84, .poissons_ratio = 0.40},
            .plastic = {.enabled = true,
                        .table = {{31.36, 0.0},
                                  {29.0, 0.0005},
                                  {32.0, 0.001},
                                  {39.0, 0.002},
                                  {45.5, 0.006}}},
            .hyperelastic = {},
        },
        MaterialDefinition{
            .name = "Bambu PETG",
            .density = {},
            .elastic = {.enabled = true, .youngs_modulus = 1703.65, .poissons_ratio = 0.33},
            .plastic = {.enabled = true,
                        .table = {{31.02, 0.0},
                                  {20.0, 0.0005},
                                  {22.0, 0.001},
                                  {27.0, 0.002},
                                  {32.0, 0.006}}},
            .hyperelastic = {},
        },
    };
}

std::array<MaterialDefinition, 2> default_material_pair() {
    auto materials = builtin_materials();
    return {std::move(materials[0]), std::move(materials[1])};
}

} // namespace mbs::domain
