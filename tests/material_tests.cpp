#include "TestSupport.hpp"
#include "mbs/domain/Material.hpp"

#include <map>

int main() {
    int failures = 0;

    const auto builtins = mbs::domain::builtin_materials();
    MBS_CHECK(builtins.size() == 2);
    MBS_CHECK(builtins[0].name == "Bambu PLA-CF");
    MBS_CHECK(builtins[1].name == "Bambu PETG");
    MBS_CHECK(builtins[0].plastic.table.back().yield_stress == 45.5);
    MBS_CHECK(builtins[1].plastic.table.back().plastic_strain == 0.006);
    MBS_CHECK(builtins[0].is_valid());
    MBS_CHECK(builtins[1].is_valid());

    mbs::domain::MaterialLibrary library{.materials = builtins};
    MBS_CHECK(library.is_valid());
    library.materials[1].name = "  bambu pla-cf  ";
    MBS_CHECK(!library.is_valid());

    auto invalid_plastic = builtins[0];
    invalid_plastic.elastic.enabled = false;
    MBS_CHECK(!invalid_plastic.is_valid());

    MBS_CHECK(mbs::domain::coefficient_names(mbs::domain::HyperelasticModel::mooney_rivlin) ==
              std::vector<std::string>({"C10", "C01", "D1"}));
    MBS_CHECK(mbs::domain::coefficient_names(mbs::domain::HyperelasticModel::ogden, 2) ==
              std::vector<std::string>({"mu1", "alpha1", "mu2", "alpha2", "D1", "D2"}));

    mbs::domain::MaterialDefinition hyper{
        .name = "Hyper",
        .density = {},
        .elastic = {.enabled = false},
        .plastic = {},
        .hyperelastic =
            {
                .enabled = true,
                .model = mbs::domain::HyperelasticModel::neo_hooke,
                .order = 1,
                .coefficients = {{"C10", 1.0}, {"D1", 0.0}},
            },
    };
    MBS_CHECK(hyper.is_valid());
    hyper.hyperelastic.coefficients["D1"] = -1.0;
    MBS_CHECK(!hyper.is_valid());

    return failures == 0 ? 0 : 1;
}
