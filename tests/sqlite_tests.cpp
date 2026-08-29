#include "TestSupport.hpp"
#include "mbs/application/TaskLifecycleService.hpp"
#include "mbs/application/TaskService.hpp"
#include "mbs/infrastructure/runtime/WorkerTaskBridge.hpp"
#include "mbs/infrastructure/sqlite/Database.hpp"
#include "mbs/infrastructure/sqlite/DatabaseManager.hpp"
#include "mbs/infrastructure/sqlite/Repositories.hpp"
#include "mbs/runtime/EventEnvelope.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                mbs::infrastructure::sqlite::generate_id("mbs4-sqlite-test")) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class IdGeneratorStub final : public mbs::application::IIdGenerator {
  public:
    std::string generate(const std::string_view prefix) override {
        ++next_;
        return std::string{prefix} + '-' + std::to_string(next_);
    }

  private:
    int next_{};
};

mbs::domain::Sample make_sample(std::string id, const int serial) {
    mbs::domain::Sample sample;
    sample.id = std::move(id);
    sample.project_id = "default";
    sample.dataset = mbs::domain::DatasetKind::mbs;
    sample.serial = serial;
    sample.parameters = {
        .lambda = 0.2, .mu = 0.3, .kappa = 0.4, .beta = -0.5, .phase_x = 0.1, .phase_y = 0.2};
    sample.source = mbs::domain::DesignSource::manual;
    sample.status = "created";
    return sample;
}

bool throws_sqlite(const auto& callable) {
    try {
        callable();
    } catch (const mbs::infrastructure::sqlite::SqliteError&) {
        return true;
    }
    return false;
}

void test_schema_and_backup(int& failures) {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "mbs.sqlite3";
    mbs::infrastructure::sqlite::DatabaseManager manager{database};
    MBS_CHECK(manager.ensure());
    MBS_CHECK(!manager.ensure());
    const auto status = manager.status();
    MBS_CHECK(status.schema_version == 2);
    MBS_CHECK(status.integrity == "ok");
    MBS_CHECK(status.journal_mode == "wal");
    MBS_CHECK(status.table_counts.at("schema_migrations") == 2);

    mbs::infrastructure::sqlite::SqliteSampleRepository samples{database};
    samples.save(make_sample("mbs-backup", 1));

    const auto backup = temporary.path() / "backup.sqlite3";
    manager.backup_to(backup);
    mbs::infrastructure::sqlite::DatabaseManager backup_manager{backup};
    const auto backup_status = backup_manager.status();
    MBS_CHECK(backup_status.schema_version == 2);
    MBS_CHECK(backup_status.integrity == "ok");
    MBS_CHECK(backup_status.table_counts.at("samples") == 1);
}

void test_incremental_schema_upgrade(int& failures) {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "upgrade.sqlite3";
    mbs::infrastructure::sqlite::DatabaseManager manager{database};
    static_cast<void>(manager.ensure());
    {
        mbs::infrastructure::sqlite::Connection connection{database};
        mbs::infrastructure::sqlite::Transaction transaction{connection};
        connection.execute("DROP TABLE metrics; DROP TABLE artifacts; DROP TABLE tasks; "
                           "DROP TABLE runs; DROP TABLE optimization_observations; "
                           "DELETE FROM schema_migrations WHERE version=2; PRAGMA user_version=1");
        transaction.commit();
    }
    MBS_CHECK(!manager.ensure());
    const auto status = manager.status();
    MBS_CHECK(status.schema_version == 2);
    MBS_CHECK(status.table_counts.at("schema_migrations") == 2);
    MBS_CHECK(status.integrity == "ok");
}

void test_samples_and_rollback(int& failures) {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "samples.sqlite3";
    mbs::infrastructure::sqlite::SqliteSampleRepository samples{database};
    auto first = make_sample("mbs-0001", 1);
    first.mesh.sizing_mode = mbs::domain::SurfaceSizingMode::curvature_adaptive;
    first.mesh.surface_tolerance_percent = 0.35;
    first.mesh.minimum_edge_percent = 0.8;
    first.mesh.maximum_edge_percent = 4.2;
    first.mesh.remesh_iterations = 4;
    first.mesh.feature_angle_degrees = 35.0;
    first.mesh.sharpen = true;
    first.mesh.simplify = true;
    first.mesh.simplify_keep_ratio = 0.72;
    first.mesh.repair_rounds = 3;
    first.mesh.tetgen.target_edge_length_mm = 0.4;
    first.mesh.tetgen.optimization_level = 4;
    first.artifact_directory = "G:/MBS/1-original";
    samples.save(first);
    auto loaded = samples.list(mbs::domain::DatasetKind::mbs);
    MBS_CHECK(loaded.size() == 1);
    MBS_CHECK(loaded[0].parameters.beta == -0.5);
    MBS_CHECK(loaded[0].mesh.estimated_grid_points() == first.mesh.estimated_grid_points());
    MBS_CHECK(loaded[0].mesh.sizing_mode ==
              mbs::domain::SurfaceSizingMode::curvature_adaptive);
    MBS_CHECK(loaded[0].mesh.surface_tolerance_percent == 0.35);
    MBS_CHECK(loaded[0].mesh.minimum_edge_percent == 0.8);
    MBS_CHECK(loaded[0].mesh.maximum_edge_percent == 4.2);
    MBS_CHECK(loaded[0].mesh.remesh_iterations == 4);
    MBS_CHECK(loaded[0].mesh.feature_angle_degrees == 35.0);
    MBS_CHECK(loaded[0].mesh.sharpen);
    MBS_CHECK(loaded[0].mesh.simplify);
    MBS_CHECK(loaded[0].mesh.simplify_keep_ratio == 0.72);
    MBS_CHECK(loaded[0].mesh.repair_rounds == 3);
    MBS_CHECK(loaded[0].mesh.tetgen.target_edge_length_mm == 0.4);
    MBS_CHECK(loaded[0].mesh.tetgen.optimization_level == 4);
    {
        mbs::infrastructure::sqlite::Connection connection{database};
        mbs::infrastructure::sqlite::Statement version{connection, "PRAGMA user_version"};
        MBS_CHECK(version.step());
        MBS_CHECK(version.column_int(0) == 2);
    }

    mbs::infrastructure::sqlite::SqliteTaskLifecycleStore lifecycle_store{database};
    IdGeneratorStub ids;
    mbs::application::TaskLifecycleService lifecycle{lifecycle_store, ids};
    const auto execution = lifecycle.start("simulation", "{}", "mbs-0001");

    first.status = "simulated";
    first.source = mbs::domain::DesignSource::bayesian_optimization;
    first.artifact_directory = "G:/MBS/should-not-overwrite-existing-artifact";
    first.parameters.phase_x = 0.42;
    first.mesh.resolution = 64;
    first.mesh.tetgen.order = 2;
    const std::array replacement{first};
    samples.replace(mbs::domain::DatasetKind::mbs, replacement);
    const auto replaced = samples.list(mbs::domain::DatasetKind::mbs)[0];
    MBS_CHECK(replaced.status == "simulated");
    MBS_CHECK(replaced.source == mbs::domain::DesignSource::manual);
    MBS_CHECK(replaced.artifact_directory == std::optional<std::string>{"G:/MBS/1-original"});
    MBS_CHECK(replaced.parameters.phase_x == 0.42);
    MBS_CHECK(replaced.mesh.resolution == 64);
    MBS_CHECK(replaced.mesh.tetgen.order == 2);
    mbs::infrastructure::sqlite::SqliteTaskRepository tasks{database};
    MBS_CHECK(tasks.find(execution.task_id)->sample_id == "mbs-0001");

    const auto before = samples.list(mbs::domain::DatasetKind::mbs);
    auto duplicate_serial = make_sample("mbs-0002", 1);
    const std::array invalid{first, duplicate_serial};
    MBS_CHECK(throws_sqlite([&] { samples.replace(mbs::domain::DatasetKind::mbs, invalid); }));
    const auto after = samples.list(mbs::domain::DatasetKind::mbs);
    MBS_CHECK(after.size() == before.size());
    MBS_CHECK(after[0].id == before[0].id);
    MBS_CHECK(after[0].status == before[0].status);
}

void test_material_and_optimization_data(int& failures) {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "data.sqlite3";
    mbs::infrastructure::sqlite::SqliteSampleRepository samples{database};
    samples.save(make_sample("mbs-0001", 1));

    mbs::infrastructure::sqlite::SqliteMaterialRepository materials{database};
    materials.replace({.materials = mbs::domain::builtin_materials()});
    const auto loaded = materials.load();
    MBS_CHECK(loaded.materials.size() == 2);
    MBS_CHECK(loaded.materials[0].plastic.table.back().yield_stress == 45.5);

    mbs::infrastructure::sqlite::SqliteOptimizationStateRepository optimization{database};
    optimization.save_pending_json("{\"candidate\":[0.2,0.3,0.4,-0.5]}");
    MBS_CHECK(optimization.load_pending_json().has_value());
    optimization.save_observation({
        .id = "observation-1",
        .sample_id = "mbs-0001",
        .objective = -12.5,
        .parameters =
            {.lambda = 0.2, .mu = 0.3, .kappa = 0.4, .beta = -0.5, .phase_x = 0.1, .phase_y = 0.2},
    });
    MBS_CHECK(optimization.observations().size() == 1);
    MBS_CHECK(optimization.observations()[0].objective == -12.5);
    optimization.delete_observation("mbs-0001");
    MBS_CHECK(optimization.observations().empty());
    optimization.clear_pending();
    MBS_CHECK(!optimization.load_pending_json().has_value());

    mbs::infrastructure::sqlite::SqliteArtifactRepository artifacts{database};
    mbs::domain::ArtifactRef artifact;
    artifact.kind = "surface";
    artifact.uri = "A.vtp";
    artifact.sample_id = "mbs-0001";
    const auto artifact_id = artifacts.register_artifact(artifact);
    MBS_CHECK(!artifact_id.empty());
    mbs::infrastructure::sqlite::SqliteMetricRepository metrics{database};
    mbs::domain::Metric metric;
    metric.name = "proof_stress";
    metric.value = 12.5;
    metric.unit = "MPa";
    metric.sample_id = "mbs-0001";
    const auto metric_id = metrics.record_metric(metric);
    MBS_CHECK(!metric_id.empty());
}

void test_task_lifecycle_atomicity_and_recovery(int& failures) {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "tasks.sqlite3";
    mbs::infrastructure::sqlite::SqliteSampleRepository samples{database};
    samples.save(make_sample("mbs-0001", 1));

    mbs::infrastructure::sqlite::SqliteTaskLifecycleStore store{database};
    IdGeneratorStub ids;
    mbs::application::TaskLifecycleService lifecycle{store, ids};
    const auto execution = lifecycle.start("postprocess", "{\"source\":\"test\"}", "mbs-0001");
    mbs::infrastructure::sqlite::SqliteTaskRepository tasks{database};
    MBS_CHECK(tasks.find(execution.task_id)->status == mbs::domain::TaskStatus::running);

    MBS_CHECK(throws_sqlite([&] {
        store.finish(execution.task_id, "missing-run", mbs::domain::TaskStatus::succeeded, {});
    }));
    const auto after_rollback = tasks.find(execution.task_id);
    MBS_CHECK(after_rollback->status == mbs::domain::TaskStatus::running);
    MBS_CHECK(after_rollback->progress == 0.0);

    mbs::application::WorkerEvent event;
    event.kind = mbs::application::WorkerEventKind::completed;
    event.task_kind = "postprocess";
    event.progress = 75.0;
    event.artifact_uris = {{"result_path", "result.json"}};
    event.proof_stress = 12.5;
    event.result_json = "{\"proof_stress\":12.5}";
    lifecycle.record_event(execution, event, "mbs-0001");
    lifecycle.finish(execution, mbs::domain::TaskStatus::succeeded);
    const auto completed = tasks.find(execution.task_id);
    MBS_CHECK(completed->status == mbs::domain::TaskStatus::succeeded);
    MBS_CHECK(completed->progress == 1.0);

    mbs::infrastructure::sqlite::DatabaseManager manager{database};
    const auto status = manager.status();
    MBS_CHECK(status.table_counts.at("runs") == 1);
    MBS_CHECK(status.table_counts.at("tasks") == 1);
    MBS_CHECK(status.table_counts.at("artifacts") == 1);
    MBS_CHECK(status.table_counts.at("metrics") == 1);

    const auto bridged_execution = lifecycle.start("animation", "{}", "mbs-0001");
    mbs::infrastructure::runtime::WorkerTaskBridge bridge{lifecycle, bridged_execution, "mbs-0001"};
    const mbs::runtime::EventEnvelope bridge_event{
        .event = "completed",
        .task_id = bridged_execution.task_id,
        .run_id = bridged_execution.run_id,
        .task_kind = "animation",
        .sample_id = "mbs-0001",
        .message = "animation ready",
        .progress = 1.0,
        .artifact_uris = {{"animation_manifest", "animation.json"}},
        .proof_stress = std::nullopt,
        .result_json = {},
    };
    MBS_CHECK(bridge.consume(bridge_event.encode()).kind == mbs::runtime::WorkerLineKind::event);
    MBS_CHECK(bridge.finish(0, false).status == mbs::runtime::WorkerTerminalStatus::succeeded);
    MBS_CHECK(tasks.find(bridged_execution.task_id)->status == mbs::domain::TaskStatus::succeeded);

    const auto interrupted = lifecycle.start("design", "{}", "mbs-0001");
    static_cast<void>(interrupted);
    MBS_CHECK(lifecycle.recover_interrupted() == 1);
}

std::size_t line_count(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return static_cast<std::size_t>(std::count(std::istreambuf_iterator<char>{input},
                                               std::istreambuf_iterator<char>{}, '\n'));
}

void test_legacy_v2_compatibility(int& failures) {
    const auto* fixture_value = std::getenv("MBS_LEGACY_DB_FIXTURE");
    if (fixture_value == nullptr || *fixture_value == '\0') {
        std::cout << "[sqlite] legacy v2 fixture not configured; skipped\n";
        return;
    }
    TemporaryDirectory temporary;
    const std::filesystem::path fixture{fixture_value};
    const auto database = temporary.path() / "legacy-v2-copy.sqlite3";
    std::filesystem::copy_file(fixture, database);
    const auto exports = temporary.path() / "exports";

    mbs::infrastructure::sqlite::SqliteSampleRepository samples{database, "default", exports};
    mbs::infrastructure::sqlite::SqliteOptimizationStateRepository optimization{database,
                                                                                 "default",
                                                                                 exports};
    mbs::infrastructure::sqlite::SqliteMaterialRepository materials{database};
    std::cout << "[sqlite] legacy copy: repositories opened\n" << std::flush;
    auto mbs_samples = samples.list(mbs::domain::DatasetKind::mbs);
    MBS_CHECK(mbs_samples.size() == 25);
    MBS_CHECK(optimization.observations().size() == 25);
    MBS_CHECK(materials.load().materials.size() == 2);
    MBS_CHECK(mbs_samples.front().id == "mbs-0001");
    MBS_CHECK(mbs_samples.front().source == mbs::domain::DesignSource::existing);
    MBS_CHECK(mbs_samples.front().artifact_directory.has_value());
    MBS_CHECK(mbs_samples.front().artifact_directory->find("1-1_0_0_0") != std::string::npos);
    std::cout << "[sqlite] legacy copy: mapped 25 samples/observations and 2 materials\n"
              << std::flush;

    using mbs::infrastructure::sqlite::Connection;
    using mbs::infrastructure::sqlite::Statement;
    std::array<std::string, 6> preserved;
    {
        Connection before_connection{database};
        Statement source_counts{before_connection,
                                "SELECT SUM(source='legacy'),SUM(source='bo') FROM samples "
                                "WHERE project_id='default' AND dataset='mbs'"};
        MBS_CHECK(source_counts.step());
        MBS_CHECK(source_counts.column_int(0) == 20);
        MBS_CHECK(source_counts.column_int(1) == 5);
        Statement before{before_connection,
                         "SELECT source,artifact_dir,created_at,legacy_json,tet_generated,"
                         "last_simulation_dir FROM samples WHERE sample_id='mbs-0001'"};
        MBS_CHECK(before.step());
        preserved = {before.column_text(0), before.column_text(1), before.column_text(2),
                     before.column_text(3), before.column_text(4), before.column_text(5)};
    }
    std::cout << "[sqlite] legacy copy: metadata snapshot captured\n" << std::flush;

    mbs_samples.front().parameters.beta += 0.001;
    mbs_samples.front().status = "compatibility_test";
    samples.replace(mbs::domain::DatasetKind::mbs, mbs_samples);
    std::cout << "[sqlite] legacy copy: edited table saved\n" << std::flush;
    Connection after_connection{database};
    Statement after{after_connection,
                    "SELECT source,artifact_dir,created_at,legacy_json,tet_generated,"
                    "last_simulation_dir,bta,status FROM samples WHERE sample_id='mbs-0001'"};
    MBS_CHECK(after.step());
    for (int column = 0; column < 6; ++column) {
        MBS_CHECK(after.column_text(column) == preserved[static_cast<std::size_t>(column)]);
    }
    MBS_CHECK(after.column_double(6) == mbs_samples.front().parameters.beta);
    MBS_CHECK(after.column_text(7) == "compatibility_test");
    MBS_CHECK(line_count(exports / "mbs_guess.csv") == 26);
    std::cout << "[sqlite] legacy copy: preservation/export verified\n" << std::flush;
}

} // namespace

int main() {
    int failures = 0;
    std::cout << "[sqlite] schema and backup\n" << std::flush;
    test_schema_and_backup(failures);
    std::cout << "[sqlite] incremental schema upgrade\n" << std::flush;
    test_incremental_schema_upgrade(failures);
    std::cout << "[sqlite] samples and rollback\n" << std::flush;
    test_samples_and_rollback(failures);
    std::cout << "[sqlite] material and optimization data\n" << std::flush;
    test_material_and_optimization_data(failures);
    std::cout << "[sqlite] task lifecycle and recovery\n" << std::flush;
    test_task_lifecycle_atomicity_and_recovery(failures);
    std::cout << "[sqlite] Mechanical-Bonding-Structure 3.0 schema v2 compatibility\n"
              << std::flush;
    test_legacy_v2_compatibility(failures);
    std::cout << "[sqlite] completed\n" << std::flush;
    return failures == 0 ? 0 : 1;
}
