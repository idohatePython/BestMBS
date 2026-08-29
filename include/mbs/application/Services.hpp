#pragma once

#include "mbs/application/Ports.hpp"

namespace mbs::application {

class DatasetService final {
  public:
    explicit DatasetService(ISampleRepository& repository) noexcept;

    [[nodiscard]] std::vector<domain::Sample> list(domain::DatasetKind dataset) const;
    void replace(domain::DatasetKind dataset, std::span<const domain::Sample> samples);
    void save(const domain::Sample& sample);

  private:
    ISampleRepository& repository_;
};

class MaterialService final {
  public:
    explicit MaterialService(IMaterialRepository& repository) noexcept;

    [[nodiscard]] domain::MaterialLibrary load() const;
    void replace(const domain::MaterialLibrary& library);

  private:
    IMaterialRepository& repository_;
};

class OptimizationStateService final {
  public:
    explicit OptimizationStateService(IOptimizationStateRepository& repository) noexcept;

    [[nodiscard]] std::optional<std::string> load_pending_json() const;
    void save_pending_json(std::string_view payload);
    void clear_pending();
    [[nodiscard]] std::vector<domain::OptimizationObservation> observations() const;
    void save_observation(const domain::OptimizationObservation& observation);
    void delete_observation(std::string_view sample_id);

  private:
    IOptimizationStateRepository& repository_;
};

class ResultCatalogService final {
  public:
    ResultCatalogService(IArtifactRepository& artifacts, IMetricRepository& metrics) noexcept;
    [[nodiscard]] std::string register_artifact(const domain::ArtifactRef& artifact);
    [[nodiscard]] std::string record_metric(const domain::Metric& metric);

  private:
    IArtifactRepository& artifacts_;
    IMetricRepository& metrics_;
};

class DesignService final {
  public:
    explicit DesignService(IGeometryEngine& engine) noexcept;
    [[nodiscard]] std::vector<domain::ArtifactRef> generate(const domain::DesignConfig& config);

  private:
    IGeometryEngine& engine_;
};

class SimulationService final {
  public:
    explicit SimulationService(ISolverBackend& backend) noexcept;
    void simulate(const domain::SimulationConfig& config, const EventSink& event_sink = {});
    void cancel(std::string_view task_id);

  private:
    ISolverBackend& backend_;
};

class PostprocessService final {
  public:
    explicit PostprocessService(IResultPostProcessor& processor) noexcept;
    [[nodiscard]] std::vector<domain::Metric> process(const domain::PostprocessConfig& config);

  private:
    IResultPostProcessor& processor_;
};

class OptimizationService final {
  public:
    explicit OptimizationService(IOptimizationEngine& engine) noexcept;
    [[nodiscard]] domain::DesignParameters
    propose(const domain::BayesianOptimizationConfig& config,
            std::span<const domain::OptimizationObservation> observations);

  private:
    IOptimizationEngine& engine_;
};

} // namespace mbs::application
