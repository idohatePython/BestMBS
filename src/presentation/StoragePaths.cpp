#include "presentation/StoragePaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace mbs::presentation::storage {
namespace {

QString environment_path(const char* name, const QString& fallback) {
    const auto configured = qEnvironmentVariable(name).trimmed();
    return QDir::cleanPath(configured.isEmpty() ? fallback : configured);
}

QString application_data_root() {
    auto path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (path.isEmpty()) {
        path = QDir{QDir::homePath()}.filePath(QStringLiteral(".bestmbs"));
    }
    return QDir::cleanPath(path);
}

} // namespace

QString project_root() {
    const auto configured = qEnvironmentVariable("MBS_PROJECT_ROOT").trimmed();
    if (!configured.isEmpty()) {
        return QDir::cleanPath(configured);
    }
    const auto development_root = QDir::cleanPath(QStringLiteral(MBS_PROJECT_SOURCE_DIR));
    if (QFileInfo{development_root}.isDir()) {
        return development_root;
    }
    return application_data_root();
}

QString database_file() {
    const auto configured = qEnvironmentVariable("MBS_DB_PATH").trimmed();
    if (!configured.isEmpty()) {
        return QDir::cleanPath(configured);
    }
    const QDir parent{QFileInfo{project_root()}.absolutePath()};
    const auto shared_v3 = parent.filePath(
        QStringLiteral("Mechanical-Bonding-Structure 3.0/data/mbs.sqlite3"));
    if (QFileInfo{shared_v3}.isFile()) {
        return QDir::cleanPath(shared_v3);
    }
    return QDir{project_root()}.filePath(QStringLiteral("data/mbs.sqlite3"));
}

QString artifact_root() {
    const auto configured = qEnvironmentVariable("MBS_ARTIFACT_ROOT").trimmed();
    if (!configured.isEmpty()) {
        return QDir::cleanPath(configured);
    }
    if (QFileInfo{QStringLiteral("G:/")}.isDir()) {
        return QStringLiteral("G:/MBS");
    }
    auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) {
        documents = QDir::homePath();
    }
    return QDir{documents}.filePath(QStringLiteral("BestMBS"));
}

QString staging_root() {
    return environment_path("MBS_STAGING_ROOT",
                            QDir{artifact_root()}.filePath(QStringLiteral("_staging")));
}

QString simulation_run_root() {
    return environment_path("MBS_SIMULATION_RUN_ROOT",
                            QDir{artifact_root()}.filePath(QStringLiteral("_simulation_runs")));
}

QString gui_cache_root() {
    return environment_path("MBS_GUI_CACHE_ROOT",
                            QDir{artifact_root()}.filePath(QStringLiteral("_gui_cache")));
}

QString scratch_root() {
    return environment_path("MBS_SCRATCH_ROOT",
                            QDir{artifact_root()}.filePath(QStringLiteral("_scratch")));
}

QString temporary_workspace() {
    const auto configured = qEnvironmentVariable("MBS_TEMP_ROOT").trimmed();
    if (!configured.isEmpty()) {
        return QDir::cleanPath(configured);
    }
    if (QFileInfo{QStringLiteral("C:/temp")}.isDir()) {
        return QStringLiteral("C:/temp/TPMS11");
    }
    return QDir{QDir::tempPath()}.filePath(QStringLiteral("BestMBS/TPMS11"));
}

QString export_data_root() { return QDir{project_root()}.filePath(QStringLiteral("data")); }

QString mbs_export_file() {
    return QDir{export_data_root()}.filePath(QStringLiteral("mbs_guess.csv"));
}

QString demo_export_file() {
    return QDir{export_data_root()}.filePath(QStringLiteral("demo.csv"));
}

QString materials_export_file() {
    return QDir{export_data_root()}.filePath(QStringLiteral("materials.json"));
}

} // namespace mbs::presentation::storage
