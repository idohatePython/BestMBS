#include "TestSupport.hpp"
#include "mbs/application/Services.hpp"
#include "mbs/application/TaskLifecycleService.hpp"
#include "mbs/application/TaskService.hpp"
#include "mbs/domain/Validation.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace {

class TaskRepositoryStub final : public mbs::application::ITaskRepository {
  public:
    void create(const mbs::domain::Task& task) override { tasks.emplace(task.id, task); }

    std::optional<mbs::domain::Task> find(const std::string_view task_id) const override {
        const auto iterator = tasks.find(std::string{task_id});
        return iterator == tasks.end() ? std::nullopt
                                       : std::optional<mbs::domain::Task>{iterator->second};
    }

    void update(const mbs::domain::Task& task) override { tasks.insert_or_assign(task.id, task); }

    int interrupt_running() override {
        int count = 0;
        for (auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (task.status == mbs::domain::TaskStatus::running) {
                task.status = mbs::domain::TaskStatus::interrupted;
                ++count;
            }
        }
        return count;
    }

    std::map<std::string, mbs::domain::Task, std::less<>> tasks;
};

class SampleRepositoryStub final : public mbs::application::ISampleRepository {
  public:
    std::vector<mbs::domain::Sample> list(const mbs::domain::DatasetKind) const override {
        return samples;
    }
    void replace(const mbs::domain::DatasetKind,
                 const std::span<const mbs::domain::Sample> values) override {
        samples.assign(values.begin(), values.end());
    }
    void save(const mbs::domain::Sample& sample) override { samples.push_back(sample); }

    std::vector<mbs::domain::Sample> samples;
};

class MaterialRepositoryStub final : public mbs::application::IMaterialRepository {
  public:
    mbs::domain::MaterialLibrary load() const override { return library; }
    void replace(const mbs::domain::MaterialLibrary& value) override { library = value; }

    mbs::domain::MaterialLibrary library;
};

class OptimizationRepositoryStub final : public mbs::application::IOptimizationStateRepository {
  public:
    std::optional<std::string> load_pending_json() const override { return pending; }
    void save_pending_json(const std::string_view payload) override { pending = payload; }
    void clear_pending() override { pending.reset(); }
    std::vector<mbs::domain::OptimizationObservation> observations() const override {
        return values;
    }
    void save_observation(const mbs::domain::OptimizationObservation& observation) override {
        values.push_back(observation);
    }
    void delete_observation(const std::string_view sample_id) override {
        std::erase_if(values, [sample_id](const auto& value) { return value.sample_id == sample_id; });
    }

    std::optional<std::string> pending;
    std::vector<mbs::domain::OptimizationObservation> values;
};

class GeometryEngineStub final : public mbs::application::IGeometryEngine {
  public:
    std::vector<mbs::domain::ArtifactRef> generate(const mbs::domain::DesignConfig&) override {
        called = true;
        mbs::domain::ArtifactRef artifact;
        artifact.id = "artifact-1";
        artifact.kind = "design_output";
        artifact.uri = "mesh.vtp";
        return {artifact};
    }
    bool called{false};
};

class IdGeneratorStub final : public mbs::application::IIdGenerator {
  public:
    std::string generate(const std::string_view prefix) override {
        ++next;
        return std::string{prefix} + "-" + std::to_string(next);
    }
    int next{};
};

class LifecycleStoreStub final : public mbs::application::ITaskLifecycleStore {
  public:
    void start(const mbs::domain::Task& task_value, const mbs::domain::Run& run_value) override {
        task = task_value;
        run = run_value;
    }

    void record_event(const std::string_view, const std::string_view,
                      const std::optional<std::string>& sample,
                      const std::span<const mbs::application::ArtifactDraft> artifact_values,
                      const std::span<const mbs::application::MetricDraft> metric_values,
                      const std::optional<double> progress_value) override {
        sample_id = sample;
        artifacts.assign(artifact_values.begin(), artifact_values.end());
        metrics.assign(metric_values.begin(), metric_values.end());
        progress = progress_value;
    }

    void finish(const std::string_view, const std::string_view,
                const mbs::domain::TaskStatus status_value,
                const std::string_view error_value) override {
        final_status = status_value;
        final_error = error_value;
    }

    int recover_interrupted() override { return 3; }

    std::optional<mbs::domain::Task> task;
    std::optional<mbs::domain::Run> run;
    std::optional<std::string> sample_id;
    std::vector<mbs::application::ArtifactDraft> artifacts;
    std::vector<mbs::application::MetricDraft> metrics;
    std::optional<double> progress;
    std::optional<mbs::domain::TaskStatus> final_status;
    std::string final_error;
};

bool throws_validation(const auto& callable) {
    try {
        callable();
    } catch (const mbs::domain::ValidationError&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    int failures = 0;

    TaskRepositoryStub task_repository;
    mbs::application::TaskService tasks{task_repository};
    mbs::domain::Task queued;
    queued.id = "task-1";
    queued.kind = "design";
    tasks.create(queued);
    MBS_CHECK(tasks.transition("task-1", mbs::domain::TaskStatus::running, 0.25));
    MBS_CHECK(tasks.transition("task-1", mbs::domain::TaskStatus::running, 0.5));
    MBS_CHECK(tasks.transition("task-1", mbs::domain::TaskStatus::succeeded));
    MBS_CHECK(!tasks.transition("task-1", mbs::domain::TaskStatus::running, 0.5));
    MBS_CHECK(tasks.find("task-1")->progress == 1.0);

    SampleRepositoryStub sample_repository;
    mbs::application::DatasetService datasets{sample_repository};
    mbs::domain::Sample sample{
        .id = "mbs-0001",
        .project_id = "project-1",
        .dataset = mbs::domain::DatasetKind::mbs,
        .serial = 1,
        .parameters =
            {.lambda = 0.2, .mu = 0.3, .kappa = 0.4, .beta = -0.5, .phase_x = 0.0, .phase_y = 0.0},
        .mesh = {},
        .source = mbs::domain::DesignSource::manual,
        .status = "created",
        .artifact_directory = std::nullopt,
    };
    datasets.save(sample);
    MBS_CHECK(datasets.list(mbs::domain::DatasetKind::mbs).size() == 1);
    sample.serial = 0;
    MBS_CHECK(throws_validation([&] { datasets.save(sample); }));

    MaterialRepositoryStub material_repository;
    mbs::application::MaterialService materials{material_repository};
    materials.replace({.materials = mbs::domain::builtin_materials()});
    MBS_CHECK(materials.load().materials.size() == 2);

    GeometryEngineStub geometry;
    mbs::application::DesignService designs{geometry};
    mbs::domain::DesignConfig config;
    config.parameters = {
        .lambda = 0.2, .mu = 0.3, .kappa = 0.4, .beta = 0.0, .phase_x = 0.0, .phase_y = 0.0};
    MBS_CHECK(designs.generate(config).size() == 1);
    MBS_CHECK(geometry.called);

    LifecycleStoreStub lifecycle_store;
    IdGeneratorStub ids;
    mbs::application::TaskLifecycleService lifecycle{lifecycle_store, ids};
    const auto execution = lifecycle.start("postprocess", "{}", "mbs-0001");
    MBS_CHECK(execution.task_id == "task-1");
    MBS_CHECK(execution.run_id == "run-2");
    MBS_CHECK(lifecycle_store.task->status == mbs::domain::TaskStatus::running);

    mbs::application::WorkerEvent completed_event;
    completed_event.kind = mbs::application::WorkerEventKind::completed;
    completed_event.task_kind = "postprocess";
    completed_event.progress = 80.0;
    completed_event.artifact_uris = {{"odb_path", "result.odb"}, {"result_path", "result.json"}};
    completed_event.proof_stress = 42.5;
    completed_event.result_json = "{\"proof_stress\":42.5}";
    lifecycle.record_event(execution, completed_event, "mbs-0001");
    MBS_CHECK(lifecycle_store.sample_id == "mbs-0001");
    MBS_CHECK(lifecycle_store.artifacts.size() == 2);
    MBS_CHECK(lifecycle_store.metrics.size() == 1);
    MBS_CHECK(lifecycle_store.progress == 0.8);
    lifecycle.finish(execution, mbs::domain::TaskStatus::succeeded);
    MBS_CHECK(lifecycle_store.final_status == mbs::domain::TaskStatus::succeeded);
    MBS_CHECK(lifecycle.recover_interrupted() == 3);

    return failures == 0 ? 0 : 1;
}
