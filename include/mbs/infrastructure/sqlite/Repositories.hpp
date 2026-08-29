#pragma once

#include "mbs/application/Ports.hpp"
#include "mbs/infrastructure/sqlite/DatabaseManager.hpp"

namespace mbs::infrastructure::sqlite {

class SqliteSampleRepository final : public application::ISampleRepository {
  public:
    explicit SqliteSampleRepository(std::filesystem::path path, std::string project_id = "default",
                                    std::filesystem::path compatibility_export_directory = {});

    [[nodiscard]] std::vector<domain::Sample> list(domain::DatasetKind dataset) const override;
    void replace(domain::DatasetKind dataset, std::span<const domain::Sample> samples) override;
    void save(const domain::Sample& sample) override;
    void export_dataset(domain::DatasetKind dataset) const;

  private:
    mutable DatabaseManager manager_;
    std::filesystem::path compatibility_export_directory_;
};

class SqliteMaterialRepository final : public application::IMaterialRepository {
  public:
    explicit SqliteMaterialRepository(std::filesystem::path path,
                                      std::string project_id = "default",
                                      std::filesystem::path compatibility_export_file = {});

    [[nodiscard]] domain::MaterialLibrary load() const override;
    void replace(const domain::MaterialLibrary& library) override;
    void export_library(const domain::MaterialLibrary& library) const;

  private:
    mutable DatabaseManager manager_;
    std::filesystem::path compatibility_export_file_;
};

class SqliteOptimizationStateRepository final : public application::IOptimizationStateRepository {
  public:
    explicit SqliteOptimizationStateRepository(std::filesystem::path path,
                                               std::string project_id = "default",
                                               std::filesystem::path compatibility_export_directory = {});

    [[nodiscard]] std::optional<std::string> load_pending_json() const override;
    void save_pending_json(std::string_view payload) override;
    void clear_pending() override;
    [[nodiscard]] std::vector<domain::OptimizationObservation> observations() const override;
    void save_observation(const domain::OptimizationObservation& observation) override;
    void delete_observation(std::string_view sample_id) override;

  private:
    mutable DatabaseManager manager_;
    std::filesystem::path compatibility_export_directory_;
    void refresh_mbs_export() const;
};

class SqliteTaskRepository final : public application::ITaskRepository {
  public:
    explicit SqliteTaskRepository(std::filesystem::path path, std::string project_id = "default");

    void create(const domain::Task& task) override;
    [[nodiscard]] std::optional<domain::Task> find(std::string_view task_id) const override;
    void update(const domain::Task& task) override;
    int interrupt_running() override;

  private:
    mutable DatabaseManager manager_;
};

class SqliteTaskLifecycleStore final : public application::ITaskLifecycleStore {
  public:
    explicit SqliteTaskLifecycleStore(std::filesystem::path path,
                                      std::string project_id = "default");

    void start(const domain::Task& task, const domain::Run& run) override;
    void record_event(std::string_view task_id, std::string_view run_id,
                      const std::optional<std::string>& sample_id,
                      std::span<const application::ArtifactDraft> artifacts,
                      std::span<const application::MetricDraft> metrics,
                      std::optional<double> progress) override;
    void finish(std::string_view task_id, std::string_view run_id, domain::TaskStatus status,
                std::string_view error) override;
    int recover_interrupted() override;

  private:
    mutable DatabaseManager manager_;
};

class SqliteArtifactRepository final : public application::IArtifactRepository {
  public:
    explicit SqliteArtifactRepository(std::filesystem::path path,
                                      std::string project_id = "default");
    [[nodiscard]] std::string register_artifact(const domain::ArtifactRef& artifact) override;

  private:
    mutable DatabaseManager manager_;
};

class SqliteMetricRepository final : public application::IMetricRepository {
  public:
    explicit SqliteMetricRepository(std::filesystem::path path, std::string project_id = "default");
    [[nodiscard]] std::string record_metric(const domain::Metric& metric) override;

  private:
    mutable DatabaseManager manager_;
};

} // namespace mbs::infrastructure::sqlite
