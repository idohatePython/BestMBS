#include "mbs/application/Services.hpp"

#include "mbs/domain/Validation.hpp"

#include <cmath>
#include <stdexcept>

namespace mbs::application {

DatasetService::DatasetService(ISampleRepository& repository) noexcept : repository_(repository) {}

std::vector<domain::Sample> DatasetService::list(const domain::DatasetKind dataset) const {
    return repository_.list(dataset);
}

void DatasetService::replace(const domain::DatasetKind dataset,
                             const std::span<const domain::Sample> samples) {
    for (const auto& sample : samples) {
        domain::require_valid(sample.validation_errors());
        if (sample.dataset != dataset) {
            throw std::invalid_argument{"sample dataset does not match replacement target"};
        }
    }
    repository_.replace(dataset, samples);
}

void DatasetService::save(const domain::Sample& sample) {
    domain::require_valid(sample.validation_errors());
    repository_.save(sample);
}

MaterialService::MaterialService(IMaterialRepository& repository) noexcept
    : repository_(repository) {}

domain::MaterialLibrary MaterialService::load() const { return repository_.load(); }

void MaterialService::replace(const domain::MaterialLibrary& library) {
    domain::require_valid(library.validation_errors());
    repository_.replace(library);
}

OptimizationStateService::OptimizationStateService(
    IOptimizationStateRepository& repository) noexcept
    : repository_(repository) {}

std::optional<std::string> OptimizationStateService::load_pending_json() const {
    return repository_.load_pending_json();
}

void OptimizationStateService::save_pending_json(const std::string_view payload) {
    if (payload.empty()) {
        throw std::invalid_argument{"pending optimization payload must not be empty"};
    }
    repository_.save_pending_json(payload);
}

void OptimizationStateService::clear_pending() { repository_.clear_pending(); }

std::vector<domain::OptimizationObservation> OptimizationStateService::observations() const {
    return repository_.observations();
}

void OptimizationStateService::save_observation(
    const domain::OptimizationObservation& observation) {
    domain::require_valid(observation.parameters.validation_errors());
    if (observation.id.empty() || observation.sample_id.empty() ||
        !std::isfinite(observation.objective)) {
        throw std::invalid_argument{"optimization observation is incomplete"};
    }
    repository_.save_observation(observation);
}

void OptimizationStateService::delete_observation(const std::string_view sample_id) {
    if (sample_id.empty()) {
        throw std::invalid_argument{"optimization sample id must not be empty"};
    }
    repository_.delete_observation(sample_id);
}

ResultCatalogService::ResultCatalogService(IArtifactRepository& artifacts,
                                           IMetricRepository& metrics) noexcept
    : artifacts_(artifacts), metrics_(metrics) {}

std::string ResultCatalogService::register_artifact(const domain::ArtifactRef& artifact) {
    if (artifact.kind.empty() || artifact.uri.empty()) {
        throw std::invalid_argument{"artifact kind and URI are required"};
    }
    return artifacts_.register_artifact(artifact);
}

std::string ResultCatalogService::record_metric(const domain::Metric& metric) {
    if (metric.name.empty() || !std::isfinite(metric.value)) {
        throw std::invalid_argument{"metric name and finite value are required"};
    }
    return metrics_.record_metric(metric);
}

DesignService::DesignService(IGeometryEngine& engine) noexcept : engine_(engine) {}

std::vector<domain::ArtifactRef> DesignService::generate(const domain::DesignConfig& config) {
    domain::require_valid(config.validation_errors());
    return engine_.generate(config);
}

SimulationService::SimulationService(ISolverBackend& backend) noexcept : backend_(backend) {}

void SimulationService::simulate(const domain::SimulationConfig& config,
                                 const EventSink& event_sink) {
    domain::require_valid(config.validation_errors());
    backend_.solve(config, event_sink);
}

void SimulationService::cancel(const std::string_view task_id) {
    if (task_id.empty()) {
        throw std::invalid_argument{"task id must not be empty"};
    }
    backend_.cancel(task_id);
}

PostprocessService::PostprocessService(IResultPostProcessor& processor) noexcept
    : processor_(processor) {}

std::vector<domain::Metric> PostprocessService::process(const domain::PostprocessConfig& config) {
    domain::require_valid(config.validation_errors());
    return processor_.process(config);
}

OptimizationService::OptimizationService(IOptimizationEngine& engine) noexcept : engine_(engine) {}

domain::DesignParameters
OptimizationService::propose(const domain::BayesianOptimizationConfig& config,
                             const std::span<const domain::OptimizationObservation> observations) {
    domain::require_valid(config.validation_errors());
    for (const auto& observation : observations) {
        domain::require_valid(observation.parameters.validation_errors());
    }
    auto candidate = engine_.propose(config, observations);
    domain::require_valid(candidate.validation_errors());
    return candidate;
}

} // namespace mbs::application
