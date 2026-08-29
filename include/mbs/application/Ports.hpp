#pragma once

#include "mbs/domain/Workflow.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::application {

struct ArtifactDraft final {
    std::string kind;
    std::string uri;
    std::optional<std::string> sample_id;
    std::string run_id;
};

struct MetricDraft final {
    std::string name;
    double value{};
    std::string unit;
    std::optional<std::string> sample_id;
    std::string run_id;
    std::string details_json;
};

class ISampleRepository {
  public:
    virtual ~ISampleRepository() = default;
    [[nodiscard]] virtual std::vector<domain::Sample> list(domain::DatasetKind dataset) const = 0;
    virtual void replace(domain::DatasetKind dataset, std::span<const domain::Sample> samples) = 0;
    virtual void save(const domain::Sample& sample) = 0;
};

class IMaterialRepository {
  public:
    virtual ~IMaterialRepository() = default;
    [[nodiscard]] virtual domain::MaterialLibrary load() const = 0;
    virtual void replace(const domain::MaterialLibrary& library) = 0;
};

class IOptimizationStateRepository {
  public:
    virtual ~IOptimizationStateRepository() = default;
    [[nodiscard]] virtual std::optional<std::string> load_pending_json() const = 0;
    virtual void save_pending_json(std::string_view payload) = 0;
    virtual void clear_pending() = 0;
    [[nodiscard]] virtual std::vector<domain::OptimizationObservation> observations() const = 0;
    virtual void save_observation(const domain::OptimizationObservation& observation) = 0;
    virtual void delete_observation(std::string_view sample_id) = 0;
};

class IArtifactRepository {
  public:
    virtual ~IArtifactRepository() = default;
    [[nodiscard]] virtual std::string register_artifact(const domain::ArtifactRef& artifact) = 0;
};

class IMetricRepository {
  public:
    virtual ~IMetricRepository() = default;
    [[nodiscard]] virtual std::string record_metric(const domain::Metric& metric) = 0;
};

class ITaskRepository {
  public:
    virtual ~ITaskRepository() = default;
    virtual void create(const domain::Task& task) = 0;
    [[nodiscard]] virtual std::optional<domain::Task> find(std::string_view task_id) const = 0;
    virtual void update(const domain::Task& task) = 0;
    virtual int interrupt_running() = 0;
};

class ITaskLifecycleStore {
  public:
    virtual ~ITaskLifecycleStore() = default;
    virtual void start(const domain::Task& task, const domain::Run& run) = 0;
    virtual void record_event(std::string_view task_id, std::string_view run_id,
                              const std::optional<std::string>& sample_id,
                              std::span<const ArtifactDraft> artifacts,
                              std::span<const MetricDraft> metrics,
                              std::optional<double> progress) = 0;
    virtual void finish(std::string_view task_id, std::string_view run_id,
                        domain::TaskStatus status, std::string_view error) = 0;
    virtual int recover_interrupted() = 0;
};

class IGeometryEngine {
  public:
    virtual ~IGeometryEngine() = default;
    [[nodiscard]] virtual std::vector<domain::ArtifactRef>
    generate(const domain::DesignConfig& config) = 0;
};

using EventSink = std::function<void(std::string_view)>;

class ISolverBackend {
  public:
    virtual ~ISolverBackend() = default;
    virtual void solve(const domain::SimulationConfig& config, const EventSink& event_sink) = 0;
    virtual void cancel(std::string_view task_id) = 0;
};

class IResultPostProcessor {
  public:
    virtual ~IResultPostProcessor() = default;
    [[nodiscard]] virtual std::vector<domain::Metric>
    process(const domain::PostprocessConfig& config) = 0;
};

class IOptimizationEngine {
  public:
    virtual ~IOptimizationEngine() = default;
    [[nodiscard]] virtual domain::DesignParameters
    propose(const domain::BayesianOptimizationConfig& config,
            std::span<const domain::OptimizationObservation> observations) = 0;
};

class IIdGenerator {
  public:
    virtual ~IIdGenerator() = default;
    [[nodiscard]] virtual std::string generate(std::string_view prefix) = 0;
};

} // namespace mbs::application
