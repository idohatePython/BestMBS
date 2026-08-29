#include "TestSupport.hpp"
#include "mbs/infrastructure/sqlite/Repositories.hpp"
#include "mbs/optimization/GaussianProcessOptimizer.hpp"
#include "mbs/runtime/EventEnvelope.hpp"
#include "mbs/runtime/ProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <set>
#include <vector>

namespace {

double objective(const mbs::domain::DesignParameters& value) {
    const auto lambda = value.lambda - 0.25;
    const auto mu = value.mu - 0.30;
    const auto kappa = value.kappa - 0.70;
    const auto beta = value.beta + 0.20;
    return lambda * lambda + mu * mu + kappa * kappa + 0.5 * beta * beta - 20.0;
}

std::vector<mbs::domain::OptimizationObservation> observations() {
    std::vector<mbs::domain::OptimizationObservation> result;
    int serial = 0;
    for (const double lambda : {0.05, 0.25, 0.50, 0.75}) {
        for (const double kappa : {0.10, 0.45, 0.80}) {
            mbs::domain::DesignParameters parameters{
                .lambda = lambda,
                .mu = 0.35 * (1.0 - lambda),
                .kappa = kappa,
                .beta = -0.8 + 0.13 * static_cast<double>(serial)};
            result.push_back({.id = "observation-" + std::to_string(serial),
                              .sample_id = "sample-" + std::to_string(serial),
                              .objective = objective(parameters),
                              .parameters = parameters});
            ++serial;
        }
    }
    return result;
}

} // namespace

int main(const int argc, char** argv) {
    int failures = 0;
    mbs::optimization::GaussianProcessOptimizer optimizer;
    const auto data = observations();

    mbs::domain::BayesianOptimizationConfig initial_config;
    initial_config.initial_points = 20;
    initial_config.random_seed = 17;
    const auto initial = optimizer.propose_detailed(initial_config, data);
    MBS_CHECK(!initial.used_surrogate);
    MBS_CHECK(initial.initial_points_remaining == 8);
    MBS_CHECK(initial.parameters.is_valid());
    MBS_CHECK(initial.parameters.lambda + initial.parameters.mu <= 1.0 + 1.0e-12);
    const auto repeated_initial = optimizer.propose_detailed(initial_config, data);
    MBS_CHECK(initial.parameters.lambda == repeated_initial.parameters.lambda);
    MBS_CHECK(initial.parameters.mu == repeated_initial.parameters.mu);

    std::set<std::array<long long, 4>> acquisition_candidates;
    for (const auto acquisition :
         {mbs::domain::AcquisitionFunction::expected_improvement,
          mbs::domain::AcquisitionFunction::lower_confidence_bound,
          mbs::domain::AcquisitionFunction::probability_improvement}) {
        mbs::domain::BayesianOptimizationConfig config;
        config.acquisition = acquisition;
        config.initial_points = 0;
        config.random_seed = 7;
        config.candidate_pool = 24;
        const auto proposal = optimizer.propose_detailed(config, data);
        MBS_CHECK(proposal.used_surrogate);
        MBS_CHECK(proposal.parameters.is_valid());
        MBS_CHECK(proposal.parameters.lambda + proposal.parameters.mu <= 1.0 + 1.0e-12);
        MBS_CHECK(std::isfinite(proposal.predicted_objective));
        MBS_CHECK(proposal.predicted_standard_deviation > 0.0);
        MBS_CHECK(std::isfinite(proposal.acquisition_value));
        MBS_CHECK(proposal.to_json().find("mbs-cpp-gp-matern52") != std::string::npos);
        acquisition_candidates.insert(
            {std::llround(proposal.parameters.lambda * 10000.0),
             std::llround(proposal.parameters.mu * 10000.0),
             std::llround(proposal.parameters.kappa * 10000.0),
             std::llround(proposal.parameters.beta * 10000.0)});
    }
    MBS_CHECK(acquisition_candidates.size() >= 2);

    std::array<mbs::domain::DesignParameters, 2> queries{
        data[0].parameters,
        mbs::domain::DesignParameters{.lambda = 0.25, .mu = 0.30, .kappa = 0.70, .beta = -0.20}};
    const auto predictions = optimizer.predict(data, queries);
    MBS_CHECK(predictions.size() == queries.size());
    MBS_CHECK(std::isfinite(predictions[0].mean_objective));
    MBS_CHECK(predictions[0].standard_deviation > 0.0);

    std::vector<mbs::domain::DesignParameters> training_points;
    for (const auto& observation : data) {
        training_points.push_back(observation.parameters);
    }
    const auto fitted = optimizer.predict(data, training_points);
    double squared_error = 0.0;
    for (std::size_t index = 0; index < fitted.size(); ++index) {
        const auto error = fitted[index].mean_objective - data[index].objective;
        squared_error += error * error;
    }
    const auto training_rmse = std::sqrt(squared_error / static_cast<double>(fitted.size()));
    MBS_CHECK(training_rmse < 0.1);

    const auto slices = optimizer.slices(data, 32);
    for (const auto& axis : slices.axes) {
        MBS_CHECK(axis.size() == 32);
        MBS_CHECK(std::all_of(axis.begin(), axis.end(), [](const auto& point) {
            return std::isfinite(point.mean_objective) && point.standard_deviation > 0.0;
        }));
    }

    bool empty_rejected = false;
    try {
        static_cast<void>(optimizer.propose_detailed({}, {}));
    } catch (const std::invalid_argument&) {
        empty_rejected = true;
    }
    MBS_CHECK(empty_rejected);

    MBS_CHECK(argc == 2);
    if (argc == 2) {
        const auto directory =
            std::filesystem::temp_directory_path() / "mbs-stage7-worker-contract";
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        std::filesystem::create_directories(directory);
        const auto database = directory / "optimization.sqlite3";
        mbs::infrastructure::sqlite::SqliteOptimizationStateRepository repository{database};
        mbs::infrastructure::sqlite::SqliteSampleRepository samples{database};
        int sample_serial = 1;
        for (const auto& observation : data) {
            samples.save({.id = observation.sample_id,
                          .project_id = "default",
                          .dataset = mbs::domain::DatasetKind::mbs,
                          .serial = sample_serial++,
                          .parameters = observation.parameters,
                          .mesh = {},
                          .source = mbs::domain::DesignSource::bayesian_optimization,
                          .status = "evaluated",
                          .artifact_directory = std::nullopt});
            repository.save_observation(observation);
        }
        mbs::runtime::SystemProcessRunner processes;
        mbs::runtime::CancellationToken cancellation;
        const auto process = processes.run(
            {.executable = argv[1],
             .arguments = {"optimize", "--task-id", "stage7-e2e", "--database",
                           database.generic_string(), "--acquisition", "EI", "--initial-points",
                           "10", "--random-seed", "7", "--candidate-pool", "24"},
             .working_directory = directory,
             .environment = {},
             .cancellation_grace = std::chrono::seconds{3},
             .graceful_cancel = {},
             .on_tick = {}},
            cancellation, [](std::string_view) {});
        MBS_CHECK(process.exit_code == 0);
        const auto pending = repository.load_pending_json();
        MBS_CHECK(pending.has_value());
        MBS_CHECK(pending->find("mbs-cpp-gp-matern52") != std::string::npos);
        MBS_CHECK(pending->find("\"used_surrogate\":true") != std::string::npos);
        std::filesystem::remove_all(directory, cleanup_error);
    }
    return failures == 0 ? 0 : 1;
}
