#pragma once

#include "mbs/application/Services.hpp"
#include "mbs/infrastructure/sqlite/DatabaseManager.hpp"
#include "mbs/infrastructure/sqlite/Repositories.hpp"
#include "mbs/optimization/GaussianProcessOptimizer.hpp"

#include <filesystem>

namespace mbs::presentation {

class ApplicationContext final {
  public:
    ApplicationContext();

    [[nodiscard]] application::DatasetService& datasets() noexcept;
    [[nodiscard]] application::MaterialService& materials() noexcept;
    [[nodiscard]] application::OptimizationStateService& optimization() noexcept;
    [[nodiscard]] application::ResultCatalogService& results() noexcept;
    [[nodiscard]] application::ITaskLifecycleStore& task_lifecycle() noexcept;
    [[nodiscard]] optimization::GaussianProcessOptimizer& optimizer() noexcept;
    [[nodiscard]] infrastructure::sqlite::DatabaseStatus database_status();
    [[nodiscard]] const std::filesystem::path& database_path() const noexcept;

  private:
    std::filesystem::path database_path_;
    infrastructure::sqlite::DatabaseManager database_;
    infrastructure::sqlite::SqliteSampleRepository sample_repository_;
    infrastructure::sqlite::SqliteMaterialRepository material_repository_;
    infrastructure::sqlite::SqliteOptimizationStateRepository optimization_repository_;
    infrastructure::sqlite::SqliteArtifactRepository artifact_repository_;
    infrastructure::sqlite::SqliteMetricRepository metric_repository_;
    infrastructure::sqlite::SqliteTaskLifecycleStore task_lifecycle_store_;
    application::DatasetService dataset_service_;
    application::MaterialService material_service_;
    application::OptimizationStateService optimization_service_;
    application::ResultCatalogService result_service_;
    optimization::GaussianProcessOptimizer optimizer_;
};

} // namespace mbs::presentation
