#include "TestSupport.hpp"
#include "mbs/domain/DesignParameters.hpp"
#include "mbs/domain/Workflow.hpp"

#include <string_view>

namespace {

bool contains(const mbs::domain::ValidationErrors& errors, const std::string_view text) {
    for (const auto& error : errors) {
        if (error.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    int failures = 0;

    const mbs::domain::DesignParameters valid{
        .lambda = 0.2,
        .mu = 0.3,
        .kappa = 0.4,
        .beta = -0.5,
        .phase_x = 0.1,
        .phase_y = 0.2,
    };
    MBS_CHECK(valid.is_valid());

    auto invalid = valid;
    invalid.lambda = 0.8;
    invalid.mu = 0.3;
    MBS_CHECK(contains(invalid.validation_errors(), "mu"));

    mbs::domain::DesignConfig random_design;
    random_design.parameters = valid;
    random_design.parameters.phase_x = 5.0;
    MBS_CHECK(random_design.is_valid());
    random_design.random_phase = false;
    MBS_CHECK(contains(random_design.validation_errors(), "phase_x"));

    const mbs::domain::MeshSettings mesh;
    MBS_CHECK(mesh.is_valid());
    MBS_CHECK(mesh.estimated_grid_points() == 136ULL * 136ULL * 205ULL);
    MBS_CHECK(mesh.estimated_memory_bytes() == 136ULL * 136ULL * 205ULL * 192ULL);
    auto invalid_mesh = mesh;
    invalid_mesh.tetgen.order = 2;
    MBS_CHECK(!invalid_mesh.is_valid());

    mbs::domain::ContactSettings contact;
    contact.adjustment = mbs::domain::ContactAdjustment::tolerance;
    MBS_CHECK(!contact.is_valid());
    contact.adjustment_tolerance = 0.01;
    MBS_CHECK(contact.is_valid());

    mbs::domain::SimulationConfig simulation;
    simulation.mesh_directory = "mesh";
    simulation.work_directory = "work";
    MBS_CHECK(simulation.is_valid());
    MBS_CHECK(simulation.required_mesh_files()[0] == "tpms-tet-A.inp");
    simulation.backend = mbs::domain::SimulationBackend::abaqus;
    MBS_CHECK(simulation.required_mesh_files()[0] == "tpms-tri-A.inp");
    simulation.resources.cpu_count = 17;
    MBS_CHECK(contains(simulation.validation_errors(), "CPU"));
    simulation.resources.cpu_count = 10;
    simulation.step.minimum_increment = 0.01;
    simulation.step.initial_increment = 0.001;
    MBS_CHECK(contains(simulation.validation_errors(), "increments"));

    const mbs::domain::BayesianOptimizationConfig optimizer;
    MBS_CHECK(optimizer.is_valid());
    MBS_CHECK(mbs::domain::to_string(optimizer.acquisition) == "EI");

    mbs::domain::Task queued;
    queued.id = "task-1";
    queued.kind = "design";
    MBS_CHECK(queued.is_valid());
    MBS_CHECK(mbs::domain::transition_allowed(mbs::domain::TaskStatus::queued,
                                              mbs::domain::TaskStatus::running));
    MBS_CHECK(mbs::domain::transition_allowed(mbs::domain::TaskStatus::running,
                                              mbs::domain::TaskStatus::running));
    MBS_CHECK(!mbs::domain::transition_allowed(mbs::domain::TaskStatus::succeeded,
                                               mbs::domain::TaskStatus::running));

    return failures == 0 ? 0 : 1;
}
