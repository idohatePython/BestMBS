#include "mbs/infrastructure/sqlite/DatabaseManager.hpp"

#include "mbs/infrastructure/sqlite/Database.hpp"

#include <array>
#include <span>
#include <system_error>

namespace mbs::infrastructure::sqlite {
namespace {

constexpr std::string_view migration_1 = R"sql(
CREATE TABLE projects (
    project_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    dataset TEXT NOT NULL CHECK(dataset IN ('mbs', 'demo')),
    sample_id TEXT UNIQUE,
    serial INTEGER,
    source TEXT,
    status TEXT,
    lmd REAL, mu REAL, kpa REAL, bta REAL, result REAL, result_percent REAL,
    rnd_x REAL, rnd_y REAL, wth REAL, rep_z INTEGER, thk_p REAL,
    resolution INTEGER, len_pct REAL, max_attempts INTEGER,
    tet_generated INTEGER, artifact_dir TEXT, tet_order INTEGER,
    tet_mindihedral REAL, tet_minratio REAL, tet_nobisect INTEGER, tet_quality INTEGER,
    last_simulation_dir TEXT, created_at TEXT, updated_at TEXT, last_error TEXT,
    legacy_json TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX idx_samples_project_dataset ON samples(project_id, dataset, id);
CREATE UNIQUE INDEX idx_samples_serial ON samples(project_id, dataset, serial)
    WHERE serial IS NOT NULL;
CREATE TABLE materials (
    material_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    name TEXT NOT NULL COLLATE NOCASE,
    payload_json TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(project_id, name)
);
CREATE TABLE pending_optimization (
    project_id TEXT PRIMARY KEY REFERENCES projects(project_id) ON DELETE CASCADE,
    payload_json TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
);
)sql";

constexpr std::string_view migration_2 = R"sql(
CREATE TABLE runs (
    run_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    sample_id TEXT REFERENCES samples(sample_id) ON DELETE SET NULL,
    kind TEXT NOT NULL,
    status TEXT NOT NULL,
    request_json TEXT NOT NULL DEFAULT '{}',
    error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    started_at TEXT,
    finished_at TEXT
);
CREATE TABLE tasks (
    task_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    run_id TEXT REFERENCES runs(run_id) ON DELETE SET NULL,
    sample_id TEXT REFERENCES samples(sample_id) ON DELETE SET NULL,
    kind TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('queued','running','succeeded','failed','cancelled','interrupted')),
    progress REAL NOT NULL DEFAULT 0,
    error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    started_at TEXT,
    finished_at TEXT,
    updated_at TEXT NOT NULL
);
CREATE TABLE artifacts (
    artifact_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    sample_id TEXT REFERENCES samples(sample_id) ON DELETE SET NULL,
    run_id TEXT REFERENCES runs(run_id) ON DELETE SET NULL,
    kind TEXT NOT NULL,
    uri TEXT NOT NULL,
    size_bytes INTEGER,
    checksum TEXT,
    exists_flag INTEGER,
    created_at TEXT NOT NULL,
    UNIQUE(run_id,kind,uri)
);
CREATE TABLE metrics (
    metric_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    sample_id TEXT REFERENCES samples(sample_id) ON DELETE CASCADE,
    run_id TEXT REFERENCES runs(run_id) ON DELETE SET NULL,
    name TEXT NOT NULL,
    value REAL NOT NULL,
    unit TEXT NOT NULL DEFAULT '',
    details_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
);
CREATE TABLE optimization_observations (
    observation_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    sample_id TEXT NOT NULL UNIQUE REFERENCES samples(sample_id) ON DELETE CASCADE,
    lmd REAL NOT NULL,
    mu REAL NOT NULL,
    kpa REAL NOT NULL,
    bta REAL NOT NULL,
    objective REAL NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX idx_tasks_status ON tasks(project_id,status);
CREATE INDEX idx_runs_sample ON runs(project_id,sample_id);
CREATE INDEX idx_artifacts_run ON artifacts(run_id);
CREATE INDEX idx_metrics_run ON metrics(run_id);
)sql";

int read_schema_version(Connection& connection) {
    Statement statement{connection, "PRAGMA user_version"};
    if (!statement.step()) {
        throw SqliteError{SQLITE_CORRUPT, "PRAGMA user_version returned no row"};
    }
    return statement.column_int(0);
}

bool table_has_column(Connection& connection, const std::string_view table,
                      const std::string_view column) {
    Statement statement{connection, "SELECT 1 FROM pragma_table_info(?) WHERE name=?"};
    statement.bind(1, table);
    statement.bind(2, column);
    return statement.step();
}

void require_columns(Connection& connection, const std::string_view table,
                     const std::span<const std::string_view> columns) {
    for (const auto column : columns) {
        if (!table_has_column(connection, table, column)) {
            throw std::runtime_error{
                "SQLite schema v2 is not compatible with Mechanical-Bonding-Structure 3.0: " +
                std::string{table} + "." + std::string{column} + " is missing"};
        }
    }
}

void validate_v2_fingerprint(Connection& connection) {
    static constexpr std::array sample_columns{
        std::string_view{"sample_id"}, std::string_view{"dataset"},
        std::string_view{"lmd"},       std::string_view{"kpa"},
        std::string_view{"rnd_x"},     std::string_view{"wth"},
        std::string_view{"resolution"}, std::string_view{"artifact_dir"},
        std::string_view{"legacy_json"}};
    static constexpr std::array material_columns{std::string_view{"material_id"},
                                                  std::string_view{"payload_json"}};
    static constexpr std::array observation_columns{
        std::string_view{"observation_id"}, std::string_view{"sample_id"},
        std::string_view{"lmd"}, std::string_view{"kpa"}, std::string_view{"objective"}};
    require_columns(connection, "samples", sample_columns);
    require_columns(connection, "materials", material_columns);
    require_columns(connection, "optimization_observations", observation_columns);
}

void record_migration(Connection& connection, const int version) {
    Statement statement{connection,
                        "INSERT INTO schema_migrations(version,applied_at) VALUES(?,?)"};
    statement.bind(1, version);
    statement.bind(2, now_timestamp());
    statement.execute();
    connection.execute("PRAGMA user_version=" + std::to_string(version));
}

void apply_migrations(Connection& connection, const std::string& project_id) {
    int version = read_schema_version(connection);
    if (version > current_schema_version) {
        throw std::runtime_error{"database schema is newer than this application"};
    }
    if (version < 1) {
        Transaction transaction{connection};
        connection.execute(migration_1);
        const auto timestamp = now_timestamp();
        Statement project{connection, "INSERT INTO projects(project_id,name,created_at,updated_at) "
                                      "VALUES(?,?,?,?)"};
        project.bind(1, project_id);
        project.bind(2, "Default Project");
        project.bind(3, timestamp);
        project.bind(4, timestamp);
        project.execute();
        record_migration(connection, 1);
        transaction.commit();
        version = 1;
    }
    if (version < 2) {
        Transaction transaction{connection};
        connection.execute(migration_2);
        record_migration(connection, 2);
        transaction.commit();
        version = 2;
    }
    if (version == current_schema_version) {
        validate_v2_fingerprint(connection);
    }
    // 4.0-only extension data deliberately does not change PRAGMA user_version.
    // Mechanical-Bonding-Structure 3.0 therefore continues to recognize this as
    // its schema v2 and simply ignores the namespaced table.
    connection.execute(R"sql(
CREATE TABLE IF NOT EXISTS mbs4_mesh_options (
    sample_id TEXT PRIMARY KEY REFERENCES samples(sample_id) ON DELETE CASCADE,
    sizing_mode TEXT NOT NULL DEFAULT 'uniform',
    surface_tolerance_percent REAL NOT NULL DEFAULT 0.5,
    minimum_edge_percent REAL NOT NULL DEFAULT 1.0,
    maximum_edge_percent REAL NOT NULL DEFAULT 5.0,
    remesh_iterations INTEGER NOT NULL DEFAULT 3,
    feature_angle_degrees REAL NOT NULL DEFAULT 30.0,
    sharpen INTEGER NOT NULL DEFAULT 0,
    simplify INTEGER NOT NULL DEFAULT 0,
    simplify_keep_ratio REAL NOT NULL DEFAULT 0.8,
    repair_rounds INTEGER NOT NULL DEFAULT 2,
    tet_target_edge_length_mm REAL NOT NULL DEFAULT 0.0,
    tet_optimization_level INTEGER NOT NULL DEFAULT 2,
    updated_at TEXT NOT NULL
))sql");
}

std::string scalar_text(Connection& connection, const std::string_view sql) {
    Statement statement{connection, sql};
    if (!statement.step()) {
        throw SqliteError{SQLITE_CORRUPT, "SQLite scalar query returned no row"};
    }
    return statement.column_text(0);
}

std::filesystem::path temporary_database_path(const std::filesystem::path& target) {
    return target.parent_path() /
           (target.filename().string() + ".migrating-" + generate_id("schema"));
}

void remove_if_present(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

} // namespace

DatabaseManager::DatabaseManager(std::filesystem::path path, std::string project_id)
    : path_(std::move(path)), project_id_(std::move(project_id)) {
    if (path_.empty() || project_id_.empty()) {
        throw std::invalid_argument{"database path and project id are required"};
    }
}

bool DatabaseManager::ensure() {
    const bool created = !std::filesystem::exists(path_);
    if (!created) {
        Connection connection{path_};
        apply_migrations(connection, project_id_);
        return false;
    }

    const auto temporary = temporary_database_path(path_);
    try {
        {
            Connection connection{temporary, false};
            apply_migrations(connection, project_id_);
            if (scalar_text(connection, "PRAGMA integrity_check") != "ok") {
                throw SqliteError{SQLITE_CORRUPT, "new database failed integrity_check"};
            }
        }
        std::filesystem::rename(temporary, path_);
        Connection wal_connection{path_};
        static_cast<void>(wal_connection);
    } catch (...) {
        remove_if_present(temporary);
        remove_if_present(temporary.string() + "-journal");
        throw;
    }
    return true;
}

DatabaseStatus DatabaseManager::status() {
    static_cast<void>(ensure());
    Connection connection{path_};
    DatabaseStatus result{
        .path = path_,
        .schema_version = read_schema_version(connection),
        .integrity = scalar_text(connection, "PRAGMA quick_check"),
        .journal_mode = scalar_text(connection, "PRAGMA journal_mode"),
        .table_counts = {},
    };
    static constexpr std::array tables{
        "projects",
        "samples",
        "materials",
        "runs",
        "tasks",
        "artifacts",
        "metrics",
        "optimization_observations",
        "pending_optimization",
        "schema_migrations",
    };
    for (const auto* table : tables) {
        Statement count{connection, "SELECT COUNT(*) FROM " + std::string{table}};
        if (!count.step()) {
            throw SqliteError{SQLITE_CORRUPT, "table count returned no row"};
        }
        result.table_counts.emplace(table, count.column_int(0));
    }
    return result;
}

void DatabaseManager::backup_to(const std::filesystem::path& destination) {
    static_cast<void>(ensure());
    if (destination.empty() || std::filesystem::absolute(path_).lexically_normal() ==
                                   std::filesystem::absolute(destination).lexically_normal()) {
        throw std::invalid_argument{"backup destination must differ from database path"};
    }
    if (std::filesystem::exists(destination)) {
        throw std::invalid_argument{"backup destination already exists"};
    }
    try {
        Connection source{path_};
        Connection target{destination, false};
        sqlite3_backup* backup =
            sqlite3_backup_init(target.handle(), "main", source.handle(), "main");
        if (backup == nullptr) {
            throw SqliteError{sqlite3_extended_errcode(target.handle()),
                              sqlite3_errmsg(target.handle())};
        }
        const int step_result = sqlite3_backup_step(backup, -1);
        const int finish_result = sqlite3_backup_finish(backup);
        if (step_result != SQLITE_DONE || finish_result != SQLITE_OK) {
            throw SqliteError{finish_result != SQLITE_OK ? finish_result : step_result,
                              "SQLite backup failed"};
        }
    } catch (...) {
        remove_if_present(destination);
        throw;
    }
}

const std::filesystem::path& DatabaseManager::path() const noexcept { return path_; }

const std::string& DatabaseManager::project_id() const noexcept { return project_id_; }

} // namespace mbs::infrastructure::sqlite
