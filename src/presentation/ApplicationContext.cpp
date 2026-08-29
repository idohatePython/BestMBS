#include "presentation/ApplicationContext.hpp"

#include "presentation/StoragePaths.hpp"

#include "mbs/domain/Material.hpp"

namespace mbs::presentation {
namespace {

std::filesystem::path native_path(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

} // namespace

ApplicationContext::ApplicationContext()
    : database_path_(native_path(storage::database_file())), database_(database_path_),
      sample_repository_(database_path_, "default", native_path(storage::export_data_root())),
      material_repository_(database_path_, "default", native_path(storage::materials_export_file())),
      optimization_repository_(database_path_, "default", native_path(storage::export_data_root())),
      artifact_repository_(database_path_),
      metric_repository_(database_path_), task_lifecycle_store_(database_path_),
      dataset_service_(sample_repository_),
      material_service_(material_repository_), optimization_service_(optimization_repository_),
      result_service_(artifact_repository_, metric_repository_) {
    // ensure() returns whether the database was newly created; an existing,
    // successfully migrated database therefore returns false. Failures throw.
    static_cast<void>(database_.ensure());
    auto library = material_service_.load();
    if (library.materials.empty()) {
        library.materials = domain::builtin_materials();
        material_service_.replace(library);
    } else {
        material_repository_.export_library(library);
    }
    sample_repository_.export_dataset(domain::DatasetKind::mbs);
    sample_repository_.export_dataset(domain::DatasetKind::demo);
}

application::DatasetService& ApplicationContext::datasets() noexcept { return dataset_service_; }

application::MaterialService& ApplicationContext::materials() noexcept { return material_service_; }

application::OptimizationStateService& ApplicationContext::optimization() noexcept {
    return optimization_service_;
}

application::ResultCatalogService& ApplicationContext::results() noexcept {
    return result_service_;
}

application::ITaskLifecycleStore& ApplicationContext::task_lifecycle() noexcept {
    return task_lifecycle_store_;
}

optimization::GaussianProcessOptimizer& ApplicationContext::optimizer() noexcept {
    return optimizer_;
}

infrastructure::sqlite::DatabaseStatus ApplicationContext::database_status() {
    return database_.status();
}

const std::filesystem::path& ApplicationContext::database_path() const noexcept {
    return database_path_;
}

} // namespace mbs::presentation
