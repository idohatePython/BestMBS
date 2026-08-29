#include "presentation/Pages.hpp"

#include "presentation/ApplicationContext.hpp"
#include "presentation/MeshViewport.hpp"
#include "presentation/StoragePaths.hpp"
#include "presentation/TaskController.hpp"
#include "presentation/WorkflowWidgets.hpp"

#include "mbs/domain/DesignParameters.hpp"
#include "mbs/domain/Validation.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace mbs::presentation {
namespace {

class NoWheelDoubleSpinBox final : public QDoubleSpinBox {
  private:
    bool wheel_armed_{};

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        wheel_armed_ = event->button() == Qt::LeftButton;
        QDoubleSpinBox::mousePressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        wheel_armed_ = false;
        QDoubleSpinBox::focusOutEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override {
        if (!hasFocus() || !wheel_armed_) {
            event->ignore();
            return;
        }
        QDoubleSpinBox::wheelEvent(event);
    }
};

class NoWheelSpinBox final : public QSpinBox {
  private:
    bool wheel_armed_{};

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        wheel_armed_ = event->button() == Qt::LeftButton;
        QSpinBox::mousePressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        wheel_armed_ = false;
        QSpinBox::focusOutEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override {
        if (!hasFocus() || !wheel_armed_) {
            event->ignore();
            return;
        }
        QSpinBox::wheelEvent(event);
    }
};

class NoWheelComboBox final : public QComboBox {
  public:
    using QComboBox::QComboBox;

  private:
    bool wheel_armed_{};

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        wheel_armed_ = event->button() == Qt::LeftButton;
        QComboBox::mousePressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        wheel_armed_ = false;
        QComboBox::focusOutEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override {
        if (!hasFocus() || !wheel_armed_) {
            event->ignore();
            return;
        }
        QComboBox::wheelEvent(event);
    }
};

class GeometryComparisonDialog final : public QDialog {
  public:
    GeometryComparisonDialog(const QString& scene, const QString& geometry_directory,
                             QWidget* parent)
        : QDialog(parent) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("%1 对比视图（相机联动）").arg(scene));
        resize(1480, 820);
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(8, 8, 8, 8);
        grid->setSpacing(6);
        camera_render_timer_ = new QTimer(this);
        camera_render_timer_->setSingleShot(true);
        camera_render_timer_->setInterval(16);
        connect(camera_render_timer_, &QTimer::timeout, this, [this] {
            for (auto* linked : viewports_) {
                linked->render_linked_camera();
            }
        });
        const QDir visualization{QDir{geometry_directory}.filePath(QStringLiteral("visualization"))};
        struct Panel final {
            QString title;
            QStringList files;
        };
        std::vector<std::vector<Panel>> panels;
        if (scene == QStringLiteral("Boolean")) {
            panels = {
                {{tr("A · 原始 TPMS 截取"), {QStringLiteral("prt_a0.vtp"), QStringLiteral("prt_a0.obj")}},
                  {tr("A · 加入下盖板"), {QStringLiteral("prt_a0.vtp"), QStringLiteral("plt_a.vtp"), QStringLiteral("prt_a0.obj"), QStringLiteral("plt_a.obj")}},
                  {tr("A · 盖板并集"), {QStringLiteral("prt_a1.vtp"), QStringLiteral("prt_a1.obj")}},
                  {tr("A · 最终裁剪布尔件"), {QStringLiteral("prt_a.vtp"), QStringLiteral("prt_a.obj")}}},
                 {{tr("B · 原始 TPMS 截取"), {QStringLiteral("prt_b0.vtp"), QStringLiteral("prt_b0.obj")}},
                  {tr("B · 加入上盖板"), {QStringLiteral("prt_b0.vtp"), QStringLiteral("plt_b.vtp"), QStringLiteral("prt_b0.obj"), QStringLiteral("plt_b.obj")}},
                  {tr("B · 盖板并集"), {QStringLiteral("prt_b1.vtp"), QStringLiteral("prt_b1.obj")}},
                  {tr("B · 最终裁剪布尔件"), {QStringLiteral("prt_b.vtp"), QStringLiteral("prt_b.obj")}}}};
        } else if (scene == QStringLiteral("Remesh")) {
            panels = {
                {{tr("Part A · 布尔原网格"), {QStringLiteral("prt_a.vtp"), QStringLiteral("prt_a.obj")}},
                  {tr("Part A · CGAL 重网格"), {QStringLiteral("prt_a_rms.vtp"), QStringLiteral("prt_a_rms.obj")}}},
                 {{tr("Part B · 布尔原网格"), {QStringLiteral("prt_b.vtp"), QStringLiteral("prt_b.obj")}},
                  {tr("Part B · CGAL 重网格"), {QStringLiteral("prt_b_rms.vtp"), QStringLiteral("prt_b_rms.obj")}}}};
        } else {
            panels = {
                {{tr("Part A · 表面"), {QStringLiteral("prt_a_rms.vtp"), QStringLiteral("prt_a_rms.obj")}},
                  {tr("Part A · TetGen"), {QStringLiteral("prt_a_grid.vtu"), QStringLiteral("prt_a_grid.vtk")}},
                  {tr("Part A · 剖开四面体"), {QStringLiteral("prt_a_subgrid.vtu"), QStringLiteral("prt_a_subgrid.vtk")}}},
                 {{tr("Part B · 表面"), {QStringLiteral("prt_b_rms.vtp"), QStringLiteral("prt_b_rms.obj")}},
                  {tr("Part B · TetGen"), {QStringLiteral("prt_b_grid.vtu"), QStringLiteral("prt_b_grid.vtk")}},
                  {tr("Part B · 剖开四面体"), {QStringLiteral("prt_b_subgrid.vtu"), QStringLiteral("prt_b_subgrid.vtk")}}}};
        }
        for (std::size_t row = 0; row < panels.size(); ++row) {
            for (std::size_t column = 0; column < panels[row].size(); ++column) {
                const auto& panel = panels[row][column];
                auto* cell = new QWidget(this);
                auto* layout = new QVBoxLayout(cell);
                layout->setContentsMargins(2, 2, 2, 2);
                auto* title = new QLabel(panel.title, cell);
                title->setAlignment(Qt::AlignCenter);
                title->setObjectName(QStringLiteral("sectionTitle"));
                layout->addWidget(title);
                auto* viewport = new MeshViewport(cell);
                viewport->set_compact_mode(true);
                layout->addWidget(viewport, 1);
                bool loaded{};
                for (const auto& file : panel.files) {
                    const auto path = visualization.filePath(file);
                    if (QFileInfo::exists(path)) {
                        loaded = viewport->load_file(path, false) || loaded;
                    }
                }
                if (loaded) {
                    viewport->set_all_visible(true);
                    viewport->set_comparison_style();
                    viewports_.push_back(viewport);
                } else {
                    title->setText(panel.title + tr("（无制品）"));
                }
                grid->addWidget(cell, static_cast<int>(row), static_cast<int>(column));
            }
        }
        if (!viewports_.empty()) {
            std::array<double, 6> combined{std::numeric_limits<double>::infinity(),
                                           -std::numeric_limits<double>::infinity(),
                                           std::numeric_limits<double>::infinity(),
                                           -std::numeric_limits<double>::infinity(),
                                           std::numeric_limits<double>::infinity(),
                                           -std::numeric_limits<double>::infinity()};
            for (auto* viewport : viewports_) {
                viewport->share_camera_from(viewports_.front());
                if (const auto bounds = viewport->scene_bounds()) {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        combined[axis * 2] = std::min(combined[axis * 2], (*bounds)[axis * 2]);
                        combined[axis * 2 + 1] =
                            std::max(combined[axis * 2 + 1], (*bounds)[axis * 2 + 1]);
                    }
                }
                connect(viewport, &MeshViewport::camera_interacted, this, [this] {
                    if (!camera_render_timer_->isActive()) {
                        camera_render_timer_->start();
                    }
                });
            }
            viewports_.front()->set_isometric_parallel_camera(combined);
            for (auto* viewport : viewports_) {
                viewport->render_linked_camera();
            }
        }
    }

  private:
    std::vector<MeshViewport*> viewports_;
    QTimer* camera_render_timer_{};
};

QDoubleSpinBox* double_input(const double value, const double minimum, const double maximum,
                             const int decimals = 3, const double step = 0.05) {
    auto* input = new NoWheelDoubleSpinBox;
    input->setRange(minimum, maximum);
    input->setDecimals(decimals);
    input->setSingleStep(step);
    input->setValue(value);
    input->setKeyboardTracking(false);
    return input;
}

QSpinBox* integer_input(const int value, const int minimum, const int maximum,
                        const int step = 1) {
    auto* input = new NoWheelSpinBox;
    input->setRange(minimum, maximum);
    input->setSingleStep(step);
    input->setValue(value);
    input->setKeyboardTracking(false);
    return input;
}

QWidget* input_with_unit(QWidget* input, const QString& unit, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(input, 1);
    auto* unit_label = new QLabel(unit, row);
    unit_label->setMinimumWidth(52);
    unit_label->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(unit_label);
    return row;
}

QString application_data_root() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString artifact_root() {
    return storage::artifact_root();
}

QString temporary_workspace() {
    return storage::temporary_workspace();
}

QString sample_artifact_directory(const QString& sample_id) {
    const auto permanent = QDir{artifact_root()}.filePath(sample_id);
    if (QFileInfo::exists(permanent)) {
        return permanent;
    }
    const auto legacy = QDir{application_data_root()}.filePath(
        QStringLiteral("samples/%1").arg(sample_id));
    return QFileInfo::exists(legacy) ? legacy : permanent;
}

QString sample_artifact_directory(const domain::Sample& sample) {
    if (sample.artifact_directory.has_value() && !sample.artifact_directory->empty()) {
        return QDir::cleanPath(QString::fromStdString(*sample.artifact_directory));
    }
    return sample_artifact_directory(QString::fromStdString(sample.id));
}

QString known_sample_artifact_directory(ApplicationContext& context, const QString& sample_id) {
    const auto id = sample_id.toStdString();
    for (const auto dataset : {domain::DatasetKind::mbs, domain::DatasetKind::demo}) {
        const auto samples = context.datasets().list(dataset);
        const auto found = std::find_if(samples.begin(), samples.end(), [&id](const auto& sample) {
            return sample.id == id;
        });
        if (found != samples.end()) {
            return sample_artifact_directory(*found);
        }
    }
    return sample_artifact_directory(sample_id);
}

std::filesystem::path native_path(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

void reset_temporary_workspace() {
    const auto path = temporary_workspace();
    const QFileInfo info{path};
    if (info.fileName().compare(QStringLiteral("TPMS11"), Qt::CaseInsensitive) != 0 ||
        info.absolutePath() == info.absoluteFilePath()) {
        throw std::runtime_error{"refusing to clear an unsafe MBS temporary path: " +
                                 path.toStdString()};
    }
    QDir directory{path};
    if (directory.exists() && !directory.removeRecursively()) {
        throw std::runtime_error{"cannot clear the MBS temporary workspace: " +
                                 path.toStdString()};
    }
    if (!QDir{}.mkpath(path)) {
        throw std::runtime_error{"cannot create the MBS temporary workspace: " +
                                 path.toStdString()};
    }
}

QString abaqus_runtime_script(const QString& name) {
    const QDir application_directory{QCoreApplication::applicationDirPath()};
    const auto source_root = QDir::cleanPath(
        application_directory.filePath(QStringLiteral("../share/mbs/python")));
    const auto destination_root = QDir{temporary_workspace()}.filePath(
        QStringLiteral("_abaqus_runtime/python"));
    if (!QFileInfo{source_root}.isDir()) {
        throw std::runtime_error{"isolated Abaqus Python runtime is missing: " +
                                 source_root.toStdString()};
    }
    std::error_code error;
    std::filesystem::remove_all(native_path(destination_root), error);
    error.clear();
    std::filesystem::create_directories(native_path(QFileInfo{destination_root}.absolutePath()),
                                        error);
    if (error) {
        throw std::runtime_error{"cannot create the ASCII Abaqus runtime staging directory"};
    }
    std::filesystem::copy(native_path(source_root), native_path(destination_root),
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          error);
    if (error) {
        throw std::runtime_error{"cannot stage the Abaqus runtime in C:/temp/TPMS11: " +
                                 error.message()};
    }
    return QDir{destination_root}.filePath(
        QStringLiteral("mbs/infrastructure/abaqus/scripts/%1").arg(name));
}

QString parameter_token(const double value) {
    auto text = QString::number(std::abs(value) < 0.00005 ? 0.0 : value, 'f', 4);
    while (text.contains('.') && text.endsWith('0')) {
        text.chop(1);
    }
    if (text.endsWith('.')) {
        text.chop(1);
    }
    return text;
}

struct SampleAllocation final {
    QString id;
    int serial{};
    QString directory;
    bool reused{};
};

bool same_design_key(const domain::DesignParameters& left,
                     const domain::DesignParameters& right) {
    const auto equal = [](const double a, const double b) {
        const auto scale = std::max({1.0, std::abs(a), std::abs(b)});
        return std::abs(a - b) <= 1.0e-12 * scale;
    };
    return equal(left.lambda, right.lambda) && equal(left.mu, right.mu) &&
           equal(left.kappa, right.kappa) && equal(left.beta, right.beta) &&
           equal(left.phase_x, right.phase_x) && equal(left.phase_y, right.phase_y);
}

QString progressive_geometry_status(const std::string& current, const QString& candidate) {
    const auto advanced = current == "geometry_ready" || current == "simulation_running" ||
                          current == "simulation_succeeded";
    return advanced ? QString::fromStdString(current) : candidate;
}

SampleAllocation allocate_sample(const domain::DatasetKind dataset,
                                 const domain::DesignConfig& config,
                                 const std::vector<domain::Sample>& samples) {
    const QDir root{artifact_root()};
    int maximum{};
    for (const auto& sample : samples) {
        maximum = std::max(maximum, sample.serial);
        if (same_design_key(sample.parameters, config.parameters)) {
            return {.id = QString::fromStdString(sample.id),
                    .serial = sample.serial,
                    .directory = sample_artifact_directory(sample),
                    .reused = true};
        }
    }
    const QRegularExpression expression{
        dataset == domain::DatasetKind::mbs ? QStringLiteral("^(\\d+)-")
                                            : QStringLiteral("^demo-(\\d+)-")};
    for (const auto& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto match = expression.match(name);
        if (match.hasMatch()) {
            maximum = std::max(maximum, match.captured(1).toInt());
        }
    }
    const auto suffix = QStringLiteral("%1_%2_%3_%4")
                            .arg(parameter_token(config.parameters.lambda),
                                 parameter_token(config.parameters.mu),
                                 parameter_token(config.parameters.kappa),
                                 parameter_token(config.parameters.beta));
    const auto serial = maximum + 1;
    if (dataset == domain::DatasetKind::mbs) {
        return {.id = QStringLiteral("mbs-%1").arg(serial, 4, 10, QLatin1Char('0')),
                .serial = serial,
                .directory = root.filePath(QStringLiteral("%1-%2").arg(serial).arg(suffix))};
    }
    return {.id = QStringLiteral("demo-%1").arg(serial, 4, 10, QLatin1Char('0')),
            .serial = serial,
            .directory = root.filePath(
                QStringLiteral("demo-%1-%2").arg(serial, 4, 10, QLatin1Char('0')).arg(suffix))};
}

void archive_geometry_files(const QString& source, const QString& destination) {
    const auto replacing_existing = QFileInfo::exists(destination);
    const auto staging_root = storage::staging_root();
    if (!QDir{}.mkpath(staging_root) ||
        !QDir{}.mkpath(QFileInfo{destination}.absolutePath())) {
        throw std::runtime_error{"cannot create geometry publishing directories"};
    }
    const auto staging = QDir{staging_root}.filePath(
        QStringLiteral(".%1-%2")
            .arg(QFileInfo{destination}.fileName(),
                 QString::number(QDateTime::currentMSecsSinceEpoch())));
    if (!QDir{}.mkpath(staging)) {
        throw std::runtime_error{"cannot create geometry staging directory: " +
                                 staging.toStdString()};
    }
    const QDir input{source};
    const QDir output{staging};
    bool copied = false;
    for (const auto& name : input.entryList({QStringLiteral("*.inp"), QStringLiteral("*.obj"),
                                            QStringLiteral("*.json")},
                                           QDir::Files)) {
        if (!QFile::copy(input.filePath(name), output.filePath(name))) {
            QDir{staging}.removeRecursively();
            throw std::runtime_error{"cannot archive geometry artifact: " + name.toStdString()};
        }
        copied = true;
    }
    const auto source_visualization = input.filePath(QStringLiteral("visualization"));
    if (QFileInfo{source_visualization}.isDir()) {
        std::error_code copy_error;
        std::filesystem::copy(native_path(source_visualization),
                              native_path(output.filePath(QStringLiteral("visualization"))),
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              copy_error);
        if (copy_error) {
            QDir{staging}.removeRecursively();
            throw std::runtime_error{"cannot archive visualization artifacts: " +
                                     copy_error.message()};
        }
        copied = true;
    }
    if (!copied) {
        QDir{staging}.removeRecursively();
        throw std::runtime_error{"geometry worker produced no persistent INP/OBJ/JSON artifacts"};
    }
    std::error_code error;
    if (!replacing_existing) {
        std::filesystem::rename(native_path(staging), native_path(destination), error);
        if (error) {
            QDir{staging}.removeRecursively();
            throw std::runtime_error{"cannot publish geometry archive atomically: " +
                                     error.message()};
        }
        return;
    }

    // Rebuilding an identical natural-key design updates only geometry artifacts.
    // Abaqus ODB, proof results and animation data in the sample directory stay intact.
    const QDir staged{staging};
    QDir published{destination};
    for (const auto& name : staged.entryList(QDir::Files)) {
        const auto target = published.filePath(name);
        QFile::remove(target);
        if (!QFile::rename(staged.filePath(name), target)) {
            QDir{staging}.removeRecursively();
            throw std::runtime_error{"cannot replace geometry artifact: " + name.toStdString()};
        }
    }
    const auto staged_visualization = staged.filePath(QStringLiteral("visualization"));
    if (QFileInfo{staged_visualization}.isDir()) {
        const auto published_visualization = published.filePath(QStringLiteral("visualization"));
        QDir{published_visualization}.removeRecursively();
        std::filesystem::rename(native_path(staged_visualization),
                                native_path(published_visualization), error);
        if (error) {
            QDir{staging}.removeRecursively();
            throw std::runtime_error{"cannot replace visualization artifacts: " + error.message()};
        }
    }
    QDir{staging}.removeRecursively();
}

int cleanup_successful_abaqus_transients(const QString& directory, const QString& job_name) {
    const QDir work{directory};
    int removed = 0;
    for (const auto& suffix : {QStringLiteral(".com"), QStringLiteral(".dat"),
                               QStringLiteral(".log"), QStringLiteral(".msg"),
                               QStringLiteral(".prt"), QStringLiteral(".sim"),
                               QStringLiteral(".sta"), QStringLiteral(".lck"),
                               QStringLiteral(".023"), QStringLiteral(".mdl"),
                               QStringLiteral(".stt"), QStringLiteral(".res"),
                               QStringLiteral(".sel"), QStringLiteral(".abq"),
                               QStringLiteral(".pac"), QStringLiteral(".pes"),
                               QStringLiteral(".ipm")}) {
        const auto path = work.filePath(job_name + suffix);
        if (QFileInfo::exists(path) && QFile::remove(path)) {
            ++removed;
        }
    }
    return removed;
}

void write_json_file(const QString& path, const QJsonObject& object) {
    QDir{}.mkpath(QFileInfo{path}.absolutePath());
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error{"cannot create JSON configuration: " + path.toStdString()};
    }
    file.write(QJsonDocument{object}.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        throw std::runtime_error{"cannot atomically publish JSON configuration: " +
                                 path.toStdString()};
    }
}

QJsonObject material_json(const domain::MaterialDefinition& material) {
    QJsonArray plastic;
    for (const auto& point : material.plastic.table) {
        plastic.append(QJsonArray{point.yield_stress, point.plastic_strain});
    }
    QJsonObject coefficients;
    for (const auto& [name, value] : material.hyperelastic.coefficients) {
        coefficients.insert(QString::fromStdString(name), value);
    }
    return {{QStringLiteral("name"), QString::fromStdString(material.name)},
            {QStringLiteral("density"),
             QJsonObject{{QStringLiteral("enabled"), material.density.enabled},
                         {QStringLiteral("value"), material.density.value}}},
            {QStringLiteral("elastic"),
             QJsonObject{{QStringLiteral("enabled"), material.elastic.enabled},
                         {QStringLiteral("youngs_modulus"), material.elastic.youngs_modulus},
                         {QStringLiteral("poissons_ratio"), material.elastic.poissons_ratio}}},
            {QStringLiteral("plastic"),
             QJsonObject{{QStringLiteral("enabled"), material.plastic.enabled},
                         {QStringLiteral("table"), plastic}}},
            {QStringLiteral("hyperelastic"),
             QJsonObject{{QStringLiteral("enabled"), material.hyperelastic.enabled},
                         {QStringLiteral("model"),
                          QString::fromUtf8(domain::to_string(material.hyperelastic.model).data())},
                         {QStringLiteral("order"), material.hyperelastic.order},
                         {QStringLiteral("coefficients"), coefficients}}}};
}

QString contact_name(const domain::ContactFormulation value) {
    return value == domain::ContactFormulation::penalty ? QStringLiteral("penalty")
                                                        : QStringLiteral("frictionless");
}

QString sliding_name(const domain::SlidingFormulation value) {
    return value == domain::SlidingFormulation::finite ? QStringLiteral("finite")
                                                       : QStringLiteral("small");
}

QString adjustment_name(const domain::ContactAdjustment value) {
    switch (value) {
    case domain::ContactAdjustment::none:
        return QStringLiteral("none");
    case domain::ContactAdjustment::overclosed:
        return QStringLiteral("overclosed");
    case domain::ContactAdjustment::tolerance:
        return QStringLiteral("tolerance");
    }
    return QStringLiteral("none");
}

QJsonObject simulation_json(const domain::SimulationConfig& config) {
    return {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("sample_id"), QString::fromStdString(config.sample_id)},
            {QStringLiteral("mesh_dir"), QString::fromStdString(config.mesh_directory)},
            {QStringLiteral("work_dir"), QString::fromStdString(config.work_directory)},
            {QStringLiteral("model_name"), QStringLiteral("intlck-tpms")},
            {QStringLiteral("job_name"), QString::fromStdString(config.job_name)},
            {QStringLiteral("tet_backend"),
             QString::fromUtf8(domain::to_string(config.backend).data())},
            {QStringLiteral("abaqus_command"), QString::fromStdString(config.abaqus_command)},
            {QStringLiteral("wth"), config.width},
            {QStringLiteral("rep_z"), config.repeat_z},
            {QStringLiteral("thk_p"), config.plate_thickness},
            {QStringLiteral("step_time"), config.step.step_time},
            {QStringLiteral("tensile_displacement"), config.tensile_displacement},
            {QStringLiteral("num_cpus"), config.resources.cpu_count},
            {QStringLiteral("memory_percent"), config.resources.memory_percent},
            {QStringLiteral("scratch_dir"), QString::fromStdString(config.resources.scratch_directory)},
            {QStringLiteral("submit_job"), true},
            {QStringLiteral("monitor_until_complete"), 1},
            {QStringLiteral("abaqus_mesh"),
             QJsonObject{{QStringLiteral("use_target_size"), config.use_target_mesh_size},
                         {QStringLiteral("target_size"), config.target_mesh_size}}},
            {QStringLiteral("tolerances"),
             QJsonObject{{QStringLiteral("vertex"), std::stod(config.options.at("vertex_tolerance"))},
                         {QStringLiteral("edge"), std::stod(config.options.at("edge_tolerance"))},
                         {QStringLiteral("surface"), std::stod(config.options.at("surface_tolerance"))},
                         {QStringLiteral("angle"), std::stod(config.options.at("angle_tolerance"))}}},
            {QStringLiteral("step"),
             QJsonObject{{QStringLiteral("stabilization_magnitude"), config.step.stabilization_magnitude},
                         {QStringLiteral("adaptive_damping_ratio"), config.step.adaptive_damping_ratio},
                         {QStringLiteral("max_num_inc"), config.step.maximum_increments},
                         {QStringLiteral("initial_inc"), config.step.initial_increment},
                         {QStringLiteral("min_inc"), config.step.minimum_increment},
                         {QStringLiteral("max_inc"), config.step.maximum_increment}}},
            {QStringLiteral("contact"),
             QJsonObject{{QStringLiteral("formulation"), contact_name(config.contact.formulation)},
                         {QStringLiteral("friction_coefficient"), config.contact.friction_coefficient},
                         {QStringLiteral("sliding"), sliding_name(config.contact.sliding)},
                         {QStringLiteral("adjust_method"), adjustment_name(config.contact.adjustment)},
                         {QStringLiteral("adjust_tolerance"), config.contact.adjustment_tolerance}}},
            {QStringLiteral("materials"),
             QJsonObject{{QStringLiteral("A"), material_json(config.materials[0])},
                         {QStringLiteral("B"), material_json(config.materials[1])}}}};
}

QString path_text(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

QString validation_text(const domain::ValidationErrors& errors) {
    QStringList lines;
    for (const auto& error : errors) {
        lines.push_back(QStringLiteral("• %1").arg(QString::fromStdString(error)));
    }
    return lines.join(QLatin1Char('\n'));
}

QString pending_summary(const std::optional<std::string>& payload) {
    if (!payload.has_value()) {
        return QStringLiteral("无待建模候选");
    }
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(*payload));
    const auto object = document.object();
    const auto parameters = object.value(QStringLiteral("params")).toArray();
    if (parameters.size() != 4) {
        return QString::fromStdString(*payload);
    }
    return QStringLiteral("%1  [%2, %3, %4, %5] · 训练样本 %6 · 初始点剩余 %7%8")
        .arg(object.value(QStringLiteral("acq_func")).toString(QStringLiteral("BO")))
        .arg(parameters[0].toDouble(), 0, 'f', 4)
        .arg(parameters[1].toDouble(), 0, 'f', 4)
        .arg(parameters[2].toDouble(), 0, 'f', 4)
        .arg(parameters[3].toDouble(), 0, 'f', 4)
        .arg(object.value(QStringLiteral("evaluated_count")).toInt())
        .arg(object.value(QStringLiteral("initial_points_remaining")).toInt())
        .arg(object.value(QStringLiteral("used_surrogate")).toBool()
                 ? QStringLiteral(" · GP")
                 : QStringLiteral(" · 随机初始"));
}

QWidget* metric_card(const QString& title, QLabel*& value, QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("metricCard"));
    auto* layout = new QVBoxLayout(card);
    auto* caption = new QLabel(title, card);
    caption->setObjectName(QStringLiteral("mutedLabel"));
    value = new QLabel(QStringLiteral("—"), card);
    value->setObjectName(QStringLiteral("metricValue"));
    layout->addWidget(caption);
    layout->addWidget(value);
    return card;
}

class ConvergenceChart final : public QWidget {
  public:
    explicit ConvergenceChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(230);
    }

    void set_values(std::vector<double> values) {
        values_ = std::move(values);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor{250, 252, 255});
        const QRectF plot = QRectF{rect()}.adjusted(52.0, 28.0, -22.0, -42.0);
        painter.setPen(QPen{QColor{207, 216, 220}, 1.0});
        painter.drawRect(plot);
        painter.setPen(QColor{84, 110, 122});
        painter.drawText(QRectF{8.0, 4.0, width() - 16.0, 20.0},
                         QStringLiteral("优化目标收敛趋势（证明应力）"));
        if (values_.empty()) {
            painter.drawText(plot, Qt::AlignCenter,
                             QStringLiteral("暂无优化观测\n完成仿真或导入观测后形成 BO 收敛曲线"));
            return;
        }
        const auto [minimum, maximum] = std::minmax_element(values_.begin(), values_.end());
        const auto span = std::max(1.0e-9, *maximum - *minimum);
        QPainterPath sample_path;
        QPainterPath best_path;
        double best = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < values_.size(); ++index) {
            best = std::max(best, values_[index]);
            const auto x = values_.size() == 1
                               ? 0.5
                               : static_cast<double>(index) /
                                     static_cast<double>(values_.size() - 1);
            const auto sample_y = (values_[index] - *minimum) / span;
            const auto best_y = (best - *minimum) / span;
            const auto sample_point = QPointF{plot.left() + x * plot.width(),
                                              plot.bottom() - sample_y * plot.height()};
            const auto best_point = QPointF{plot.left() + x * plot.width(),
                                            plot.bottom() - best_y * plot.height()};
            index == 0 ? sample_path.moveTo(sample_point) : sample_path.lineTo(sample_point);
            index == 0 ? best_path.moveTo(best_point) : best_path.lineTo(best_point);
            painter.setPen(QPen{QColor{144, 164, 174}, 1.0});
            painter.setBrush(QColor{144, 164, 174});
            painter.drawEllipse(sample_point, 3.0, 3.0);
        }
        painter.setPen(QPen{QColor{144, 164, 174}, 1.5});
        painter.drawPath(sample_path);
        painter.setPen(QPen{QColor{21, 101, 192}, 3.0});
        painter.drawPath(best_path);
    }

  private:
    std::vector<double> values_;
};

class ParallelCoordinatesChart final : public QWidget {
  public:
    explicit ParallelCoordinatesChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(230);
    }

    void set_observations(std::vector<domain::OptimizationObservation> observations) {
        observations_ = std::move(observations);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor{250, 252, 255});
        const QRectF plot = QRectF{rect()}.adjusted(55.0, 34.0, -35.0, -38.0);
        painter.setPen(QColor{84, 110, 122});
        painter.drawText(QRectF{8.0, 5.0, width() - 16.0, 22.0},
                         QStringLiteral("参数平行坐标 · 颜色表示证明应力"));
        if (observations_.empty()) {
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("暂无有效优化观测"));
            return;
        }
        const std::array labels{QStringLiteral("λ"), QStringLiteral("μ"),
                                QStringLiteral("κ"), QStringLiteral("β")};
        for (std::size_t axis = 0; axis < labels.size(); ++axis) {
            const auto x = plot.left() + static_cast<double>(axis) * plot.width() / 3.0;
            painter.setPen(QPen{QColor{207, 216, 220}, 1.0});
            painter.drawLine(QPointF{x, plot.top()}, QPointF{x, plot.bottom()});
            painter.setPen(QColor{69, 90, 100});
            painter.drawText(QRectF{x - 20.0, plot.bottom() + 7.0, 40.0, 20.0},
                             Qt::AlignCenter, labels[axis]);
        }
        const auto [minimum, maximum] = std::minmax_element(
            observations_.begin(), observations_.end(), [](const auto& left, const auto& right) {
                return -left.objective < -right.objective;
            });
        const auto min_stress = -minimum->objective;
        const auto span = std::max(1.0e-12, -maximum->objective - min_stress);
        for (const auto& observation : observations_) {
            const auto& p = observation.parameters;
            const std::array normalized{p.lambda, p.mu, p.kappa, (p.beta + 1.0) / 2.0};
            const auto color_ratio = std::clamp((-observation.objective - min_stress) / span, 0.0,
                                                1.0);
            const QColor color{static_cast<int>(68.0 + 185.0 * color_ratio),
                               static_cast<int>(35.0 + 190.0 * color_ratio),
                               static_cast<int>(105.0 - 70.0 * color_ratio), 155};
            QPainterPath path;
            for (std::size_t axis = 0; axis < normalized.size(); ++axis) {
                const QPointF point{plot.left() + static_cast<double>(axis) * plot.width() / 3.0,
                                    plot.bottom() - std::clamp(normalized[axis], 0.0, 1.0) *
                                                        plot.height()};
                axis == 0 ? path.moveTo(point) : path.lineTo(point);
            }
            painter.setPen(QPen{color, 1.5});
            painter.drawPath(path);
        }
    }

  private:
    std::vector<domain::OptimizationObservation> observations_;
};

class SurrogateSliceChart final : public QWidget {
  public:
    explicit SurrogateSliceChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(300);
    }

    void set_slices(std::optional<optimization::SurrogateSlices> slices, QString message = {}) {
        slices_ = std::move(slices);
        message_ = std::move(message);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor{250, 252, 255});
        if (!slices_.has_value()) {
            painter.setPen(QColor{84, 110, 122});
            painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                             message_.isEmpty() ? QStringLiteral("暂无 GP 代理模型") : message_);
            return;
        }
        const std::array labels{QStringLiteral("λ 切片"), QStringLiteral("μ 切片"),
                                QStringLiteral("κ 切片"), QStringLiteral("β 切片")};
        for (std::size_t axis = 0; axis < 4; ++axis) {
            const auto column = static_cast<int>(axis % 2);
            const auto row = static_cast<int>(axis / 2);
            QRectF panel{10.0 + column * width() / 2.0, 8.0 + row * height() / 2.0,
                         width() / 2.0 - 18.0, height() / 2.0 - 16.0};
            const auto plot = panel.adjusted(38.0, 22.0, -10.0, -26.0);
            painter.setPen(QColor{69, 90, 100});
            painter.drawText(QRectF{panel.left(), panel.top(), panel.width(), 20.0},
                             Qt::AlignCenter, labels[axis]);
            painter.setPen(QPen{QColor{207, 216, 220}, 1.0});
            painter.drawRect(plot);
            const auto& values = slices_->axes[axis];
            if (values.empty()) {
                continue;
            }
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            for (const auto& value : values) {
                const auto stress = -value.mean_objective;
                minimum = std::min(minimum, stress - 1.96 * value.standard_deviation);
                maximum = std::max(maximum, stress + 1.96 * value.standard_deviation);
            }
            const auto span = std::max(1.0e-12, maximum - minimum);
            const auto x_min = values.front().coordinate;
            const auto x_span = std::max(1.0e-12, values.back().coordinate - x_min);
            QPainterPath mean_path;
            QPainterPath upper;
            QPainterPath lower;
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto& value = values[index];
                const auto x = plot.left() + (value.coordinate - x_min) / x_span * plot.width();
                const auto mean = -value.mean_objective;
                const auto y = [&](const double stress) {
                    return plot.bottom() - (stress - minimum) / span * plot.height();
                };
                const QPointF center{x, y(mean)};
                const QPointF high{x, y(mean + 1.96 * value.standard_deviation)};
                const QPointF low{x, y(mean - 1.96 * value.standard_deviation)};
                index == 0 ? mean_path.moveTo(center) : mean_path.lineTo(center);
                index == 0 ? upper.moveTo(high) : upper.lineTo(high);
                index == 0 ? lower.moveTo(low) : lower.lineTo(low);
            }
            auto band = upper;
            for (auto iterator = values.rbegin(); iterator != values.rend(); ++iterator) {
                const auto x = plot.left() + (iterator->coordinate - x_min) / x_span * plot.width();
                const auto stress = -iterator->mean_objective - 1.96 * iterator->standard_deviation;
                const auto y = plot.bottom() - (stress - minimum) / span * plot.height();
                band.lineTo(QPointF{x, y});
            }
            band.closeSubpath();
            painter.fillPath(band, QColor{144, 202, 249, 95});
            painter.setPen(QPen{QColor{21, 101, 192}, 2.0});
            painter.drawPath(mean_path);
        }
    }

  private:
    std::optional<optimization::SurrogateSlices> slices_;
    QString message_;
};

class StressStrainChart final : public QWidget {
  public:
    explicit StressStrainChart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(260);
    }

    void set_result(const QJsonObject& result) {
        strain_.clear();
        stress_.clear();
        offset_.clear();
        for (const auto value : result.value(QStringLiteral("strain")).toArray()) {
            strain_.push_back(value.toDouble());
        }
        for (const auto value : result.value(QStringLiteral("stress")).toArray()) {
            stress_.push_back(value.toDouble());
        }
        for (const auto value : result.value(QStringLiteral("offset_stress")).toArray()) {
            offset_.push_back(value.toDouble());
        }
        proof_strain_ = result.value(QStringLiteral("proof_strain")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        proof_stress_ = result.value(QStringLiteral("proof_stress")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor{250, 252, 255});
        const auto plot = QRectF{rect()}.adjusted(62.0, 30.0, -24.0, -48.0);
        painter.setPen(QColor{69, 90, 100});
        painter.drawText(QRectF{8.0, 4.0, width() - 16.0, 22.0},
                         QStringLiteral("ODB 应力–应变与 0.2% 偏移证明应力"));
        painter.setPen(QPen{QColor{207, 216, 220}, 1.0});
        painter.drawRect(plot);
        if (strain_.size() < 2 || stress_.size() != strain_.size()) {
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("选择 ODB 并完成后处理后显示"));
            return;
        }
        const auto max_x = std::max(1.0e-12, *std::max_element(strain_.begin(), strain_.end()));
        const auto stress_range = std::minmax_element(stress_.begin(), stress_.end());
        double min_y = std::min(0.0, *stress_range.first);
        double max_y = std::max(0.0, *stress_range.second);
        if (!offset_.empty()) {
            const auto range = std::minmax_element(offset_.begin(), offset_.end());
            min_y = std::min(min_y, *range.first);
            max_y = std::max(max_y, *range.second);
        }
        const auto span_y = std::max(1.0e-12, max_y - min_y);
        const auto point = [&](const double x, const double y) {
            return QPointF{plot.left() + x / max_x * plot.width(),
                           plot.bottom() - (y - min_y) / span_y * plot.height()};
        };
        const auto draw_curve = [&](const std::vector<double>& values, const QColor color,
                                    const Qt::PenStyle style) {
            if (values.size() != strain_.size()) {
                return;
            }
            QPainterPath path;
            for (std::size_t index = 0; index < values.size(); ++index) {
                index == 0 ? path.moveTo(point(strain_[index], values[index]))
                           : path.lineTo(point(strain_[index], values[index]));
            }
            painter.setPen(QPen{color, 2.2, style});
            painter.drawPath(path);
        };
        draw_curve(stress_, QColor{21, 101, 192}, Qt::SolidLine);
        draw_curve(offset_, QColor{239, 108, 0}, Qt::DashLine);
        if (std::isfinite(proof_strain_) && std::isfinite(proof_stress_)) {
            painter.setPen(QPen{QColor{198, 40, 40}, 2.0});
            painter.setBrush(QColor{198, 40, 40});
            painter.drawEllipse(point(proof_strain_, proof_stress_), 4.5, 4.5);
            painter.drawText(point(proof_strain_, proof_stress_) + QPointF{7.0, -7.0},
                             QStringLiteral("%1 MPa").arg(proof_stress_, 0, 'g', 6));
        }
        painter.setPen(QColor{84, 110, 122});
        painter.drawText(QRectF{plot.left(), plot.bottom() + 12.0, plot.width(), 22.0},
                         Qt::AlignCenter, QStringLiteral("工程应变"));
    }

  private:
    std::vector<double> strain_;
    std::vector<double> stress_;
    std::vector<double> offset_;
    double proof_strain_{std::numeric_limits<double>::quiet_NaN()};
    double proof_stress_{std::numeric_limits<double>::quiet_NaN()};
};

QString sample_label(const domain::Sample& sample) {
    return QStringLiteral("%1 | λ=%2 μ=%3 κ=%4 β=%5")
        .arg(QString::fromStdString(sample.id))
        .arg(sample.parameters.lambda, 0, 'f', 2)
        .arg(sample.parameters.mu, 0, 'f', 2)
        .arg(sample.parameters.kappa, 0, 'f', 2)
        .arg(sample.parameters.beta, 0, 'f', 2);
}

} // namespace

DesignPage::DesignPage(ApplicationContext& context, TaskController& controller, QWidget* parent)
    : QWidget(parent), context_(context), controller_(controller) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    auto* controls_content = new QWidget(splitter);
    auto* controls = new QVBoxLayout(controls_content);
    controls->setContentsMargins(4, 4, 8, 4);
    controls->setSpacing(6);

    auto* source_group = new QGroupBox(tr("参数来源"), controls_content);
    auto* source_form = new QFormLayout(source_group);
    source_ = new NoWheelComboBox(source_group);
    source_->addItem(tr("手动输入 → Demo 手动样本"),
                     static_cast<int>(domain::DesignSource::manual));
    source_->addItem(tr("贝叶斯候选 → MBS 研究样本"),
                     static_cast<int>(domain::DesignSource::bayesian_optimization));
    source_->addItem(tr("数据库历史样本 → 覆盖重建"),
                     static_cast<int>(domain::DesignSource::existing));
    source_form->addRow(tr("来源"), source_);
    history_ = new NoWheelComboBox(source_group);
    auto* history_refresh = new QPushButton(tr("刷新"), source_group);
    auto* history_row = new QHBoxLayout;
    history_row->addWidget(history_, 1);
    history_row->addWidget(history_refresh);
    source_form->addRow(tr("历史样本"), history_row);
    pending_ = new QLabel(tr("无待建模候选"), source_group);
    pending_->setWordWrap(true);
    source_form->addRow(tr("BO 候选"), pending_);
    acquisition_ = new NoWheelComboBox(source_group);
    acquisition_->addItems({QStringLiteral("EI"), QStringLiteral("LCB"), QStringLiteral("PI")});
    auto* generate = new QPushButton(tr("生成候选"), source_group);
    auto* load = new QPushButton(tr("载入候选"), source_group);
    auto* bo_row = new QHBoxLayout;
    bo_row->addWidget(acquisition_);
    bo_row->addWidget(generate);
    bo_row->addWidget(load);
    source_form->addRow(bo_row);
    controls->addWidget(source_group);

    auto* parameter_group = new QGroupBox(tr("TPMS 四维结构参数"), controls_content);
    auto* parameter_form = new QFormLayout(parameter_group);
    lambda_ = double_input(0.0, 0.0, 1.0);
    mu_ = double_input(0.0, 0.0, 1.0);
    kappa_ = double_input(0.0, 0.0, 1.0);
    beta_ = double_input(0.0, -1.0, 1.0);
    parameter_form->addRow(QStringLiteral("λ / lmd"), lambda_);
    parameter_form->addRow(QStringLiteral("μ / mu"), mu_);
    parameter_form->addRow(QStringLiteral("κ / kpa"), kappa_);
    parameter_form->addRow(QStringLiteral("β / bta"), beta_);
    controls->addWidget(parameter_group);

    auto* phase_group = new QGroupBox(tr("相位参数"), controls_content);
    auto* phase_form = new QFormLayout(phase_group);
    phase_x_ = double_input(0.0, 0.0, 1.0);
    phase_y_ = double_input(0.0, 0.0, 1.0);
    phase_form->addRow(tr("X 相位 (phase x)"), phase_x_);
    phase_form->addRow(tr("Y 相位 (phase y)"), phase_y_);
    controls->addWidget(phase_group);

    auto* size_group = new QGroupBox(tr("尺寸预设"), controls_content);
    auto* size_form = new QFormLayout(size_group);
    width_ = double_input(10.0, 1.0, 100.0, 2, 0.5);
    repeat_z_ = integer_input(3, 1, 10);
    plate_thickness_ = double_input(1.0, 0.1, 20.0, 2, 0.1);
    size_form->addRow(tr("RVE 边长 (RVE length)"),
                      input_with_unit(width_, QStringLiteral("mm"), size_group));
    size_form->addRow(tr("Z 重复数 (Z repetitions)"),
                      input_with_unit(repeat_z_, tr("次"), size_group));
    size_form->addRow(tr("盖板厚度 (plate thickness)"),
                      input_with_unit(plate_thickness_, QStringLiteral("mm"), size_group));
    part_b_construction_ = new NoWheelComboBox(size_group);
    part_b_construction_->addItem(
        tr("共享隐式场互补 (shared implicit phase)"),
        static_cast<int>(domain::PartBConstruction::shared_implicit_phase));
    part_b_construction_->addItem(
        tr("目标包络减 Part A (container minus Part A)"),
        static_cast<int>(domain::PartBConstruction::container_minus_part_a));
    part_b_construction_->setToolTip(
        tr("共享隐式场模式分别构造两相；目标包络减 Part A 模式直接以完整设计包络做差，A/B 外边界严格互补。"));
    size_form->addRow(tr("Part B 构造 (Part B construction)"), part_b_construction_);
    controls->addWidget(size_group);

    auto* mesh_group = new QGroupBox(tr("表面网格化"), controls_content);
    auto* mesh_layout = new QVBoxLayout(mesh_group);

    auto* extraction_group = new QGroupBox(tr("网格化"), mesh_group);
    auto* extraction_form = new QFormLayout(extraction_group);
    resolution_ = integer_input(30, 20, 200);
    sharpen_ = new QCheckBox(tr("启用网格锐化 (sharpen)"), extraction_group);
    sharpen_->setChecked(false);
    simplify_ = new QCheckBox(tr("启用网格简化 (simplify)"), extraction_group);
    simplify_->setChecked(false);
    simplify_keep_percent_ = double_input(80.0, 10.0, 100.0, 0, 5.0);
    simplify_keep_percent_->setEnabled(false);
    resolution_->setToolTip(tr("隐式场每个 RVE 边长上的采样点数；越高越能保留细节，但内存和时间增长明显。"));
    sharpen_->setToolTip(tr("检测超过特征角的锐边，约束特征折线，并仅平滑非特征区域；失败会自动回滚。"));
    simplify_->setToolTip(tr("使用 CGAL 包络约束边折叠减少三角形；拓扑或偏差验收失败会自动回滚。"));
    extraction_form->addRow(
        tr("采样分辨率 (sampling resolution)"),
        input_with_unit(resolution_, tr("点/RVE"), extraction_group));
    extraction_form->addRow(sharpen_);
    extraction_form->addRow(simplify_);
    extraction_form->addRow(
        tr("保留面比例 (retained faces)"),
        input_with_unit(simplify_keep_percent_, QStringLiteral("%"), extraction_group));
    mesh_layout->addWidget(extraction_group);

    auto* remesh_group = new QGroupBox(tr("重网格化"), mesh_group);
    auto* remesh_form = new QFormLayout(remesh_group);
    sizing_mode_ = new NoWheelComboBox(remesh_group);
    sizing_mode_->addItem(tr("均匀 (uniform)"),
                          static_cast<int>(domain::SurfaceSizingMode::uniform));
    sizing_mode_->addItem(
        tr("曲率自适应 (curvature adaptive)"),
        static_cast<int>(domain::SurfaceSizingMode::curvature_adaptive));
    sizing_mode_->setCurrentIndex(
        sizing_mode_->findData(static_cast<int>(domain::SurfaceSizingMode::curvature_adaptive)));
    edge_percent_ = double_input(5.0, 0.1, 10.0, 2, 0.1);
    surface_tolerance_ = double_input(0.5, 0.01, 5.0, 2, 0.05);
    minimum_edge_percent_ = double_input(1.0, 0.05, 10.0, 2, 0.1);
    maximum_edge_percent_ = double_input(5.0, 0.05, 20.0, 2, 0.1);
    minimum_edge_percent_->setEnabled(false);
    maximum_edge_percent_->setEnabled(false);
    remesh_iterations_ = integer_input(3, 1, 10);
    feature_angle_ = double_input(30.0, 1.0, 90.0, 1, 1.0);
    edge_percent_->setToolTip(tr("CGAL 表面重网格的目标边长，相对于 RVE 边长；越小网格越密。"));
    surface_tolerance_->setToolTip(
        tr("曲率自适应尺寸场的误差目标，同时作为简化包络的硬上限；最终全局 Hausdorff 偏差会单独验收并写入日志。"));
    minimum_edge_percent_->setToolTip(tr("曲率自适应模式允许生成的最小三角形边长。"));
    maximum_edge_percent_->setToolTip(
        tr("曲率自适应尺寸场的局部目标上限；CGAL 分裂阈值下，实际最长边可接近该值的 4/3。"));
    remesh_iterations_->setToolTip(tr("每轮执行边分裂、折叠、翻转、松弛与回投影；通常 3–5 轮。"));
    feature_angle_->setToolTip(tr("启用锐化时，二面角超过该阈值的边被识别为特征边。"));
    remesh_form->addRow(tr("尺寸策略 (sizing strategy)"), sizing_mode_);
    auto* target_edge_field =
        input_with_unit(edge_percent_, QStringLiteral("% RVE"), remesh_group);
    remesh_form->addRow(tr("目标边长 (target edge length)"), target_edge_field);
    remesh_form->addRow(
        tr("表面容差 (surface tolerance)"),
        input_with_unit(surface_tolerance_, QStringLiteral("% RVE"), remesh_group));
    auto* minimum_edge_field =
        input_with_unit(minimum_edge_percent_, QStringLiteral("% RVE"), remesh_group);
    remesh_form->addRow(
        tr("最小边长 (minimum edge length)"), minimum_edge_field);
    auto* maximum_edge_field =
        input_with_unit(maximum_edge_percent_, QStringLiteral("% RVE"), remesh_group);
    remesh_form->addRow(
        tr("最大边长 (maximum edge length)"), maximum_edge_field);
    remesh_form->addRow(
        tr("重网格迭代 (remesh iterations)"),
        input_with_unit(remesh_iterations_, tr("轮"), remesh_group));
    remesh_form->addRow(
        tr("特征角 (feature angle)"),
        input_with_unit(feature_angle_, QStringLiteral("deg"), remesh_group));
    mesh_layout->addWidget(remesh_group);

    auto* recovery_group = new QGroupBox(tr("异常处理"), mesh_group);
    auto* recovery_form = new QFormLayout(recovery_group);
    repair_rounds_ = integer_input(2, 0, 5);
    random_phase_ = new QCheckBox(
        tr("失败后重采样相位 (resample phase)"), recovery_group);
    random_phase_->setChecked(true);
    phase_x_->setEnabled(false);
    phase_y_->setEnabled(false);
    attempts_ = integer_input(20, 1, 100);
    attempts_->setEnabled(true);
    repair_rounds_->setToolTip(tr("用于自相交和退化面的 CGAL 修复上限，不会主动重采样相位。"));
    random_phase_->setToolTip(tr("只有同一相位的修复、回滚和备用后端全部失败后才重新采样 rnd_x/rnd_y。"));
    attempts_->setToolTip(tr("仅在允许重采样相位时生效；关闭重采样时实际只尝试当前相位一次。"));
    recovery_form->addRow(
        tr("拓扑修复轮数 (repair rounds)"),
        input_with_unit(repair_rounds_, tr("轮"), recovery_group));
    recovery_form->addRow(random_phase_);
    recovery_form->addRow(
        tr("最大相位尝试 (maximum attempts)"),
        input_with_unit(attempts_, tr("次"), recovery_group));
    mesh_layout->addWidget(recovery_group);

    memory_ = new QLabel(mesh_group);
    memory_->setWordWrap(true);
    mesh_layout->addWidget(memory_);
    controls->addWidget(mesh_group);

    auto* tet_group = new QGroupBox(tr("四面体化参数"), controls_content);
    auto* tet_form = new QFormLayout(tet_group);
    tetrahedralize_ = new QCheckBox(tr("生成四面体网格 (tetrahedralize)"), tet_group);
    tetrahedralize_->setChecked(true);
    tet_order_ = new NoWheelComboBox(tet_group);
    tet_order_->addItem(QStringLiteral("Linear-Tet4 (C3D4)"), 1);
    tet_order_->addItem(QStringLiteral("Quadratic-Tet10 (C3D10)"), 2);
    tet_target_edge_ = double_input(0.0, 0.0, 100.0, 3, 0.1);
    tet_target_edge_->setSpecialValueText(tr("自动"));
    tet_dihedral_ = double_input(20.0, 0.0, 89.0, 1, 1.0);
    tet_ratio_ = double_input(1.1, 1.01, 10.0, 2, 0.1);
    tet_optimization_ = integer_input(2, 0, 10);
    tet_no_bisect_ = new QCheckBox(tr("保持输入表面 (preserve input surface)"), tet_group);
    tet_no_bisect_->setChecked(true);
    tet_quality_ = new QCheckBox(tr("启用质量优化 (quality refinement)"), tet_group);
    tet_quality_->setChecked(true);
    tet_target_edge_->setToolTip(tr("0 表示 TetGen 自动决定；非零值按正四面体公式折算为 -a 最大体积约束，并非逐单元严格边长。"));
    tet_ratio_->setToolTip(tr("外接球半径与最短边之比的最大允许值；越接近 1 约束越严格。"));
    tet_dihedral_->setToolTip(tr("TetGen 质量细化所要求的最小二面角；设置过严可能显著增加单元数或失败。"));
    tet_optimization_->setToolTip(tr("TetGen -O 优化级别，范围 0–10；越高通常越慢。"));
    tet_form->addRow(tetrahedralize_);
    tet_form->addRow(tr("几何阶次 (geometric order)"), tet_order_);
    tet_form->addRow(
        tr("目标四面体边长 (target tetra edge)"),
        input_with_unit(tet_target_edge_, QStringLiteral("mm"), tet_group));
    tet_form->addRow(
        tr("最大半径边长比 (maximum radius-edge ratio)"),
        input_with_unit(tet_ratio_, QStringLiteral("—"), tet_group));
    tet_form->addRow(
        tr("最小二面角 (minimum dihedral angle)"),
        input_with_unit(tet_dihedral_, QStringLiteral("deg"), tet_group));
    tet_form->addRow(
        tr("优化级别 (optimization level)"),
        input_with_unit(tet_optimization_, QStringLiteral("—"), tet_group));
    tet_form->addRow(tet_no_bisect_);
    tet_form->addRow(tet_quality_);
    controls->addWidget(tet_group);

    auto* save_sample_button = new QPushButton(tr("保存为 SQLite Demo 样本"), controls_content);
    auto* actions = new QGridLayout;
    auto* start = new QPushButton(tr("开始设计"), controls_content);
    start->setObjectName(QStringLiteral("primaryButton"));
    auto* stop = new QPushButton(tr("停止"), controls_content);
    stop->setEnabled(false);
    auto* load_existing = new QPushButton(tr("加载已有模型"), controls_content);
    load_existing->setObjectName(QStringLiteral("loadExistingModelButton"));
    auto* open_current = new QPushButton(tr("打开当前目录"), controls_content);
    auto* contact_risk = new QPushButton(tr("接触风险检查"), controls_content);
    actions->addWidget(start, 0, 0);
    actions->addWidget(stop, 0, 1);
    actions->addWidget(load_existing, 1, 0);
    actions->addWidget(open_current, 1, 1);
    actions->addWidget(save_sample_button, 2, 0, 1, 2);
    actions->addWidget(contact_risk, 3, 0, 1, 2);
    controls->addLayout(actions);
    auto* comparisons = new QHBoxLayout;
    for (const auto& [label, object_name, folder] :
         std::array<std::tuple<QString, QString, QString>, 3>{
             std::tuple{QStringLiteral("Boolean"), QStringLiteral("comparisonButton_B"),
                        QStringLiteral("boolean")},
             std::tuple{QStringLiteral("Remesh"), QStringLiteral("comparisonButton_R"),
                        QStringLiteral("remesh")},
             std::tuple{QStringLiteral("Tetrahedralization"),
                        QStringLiteral("comparisonButton_T"),
                        QStringLiteral("tetrahedralization")}}) {
        auto* button = new QPushButton(label, controls_content);
        button->setObjectName(object_name);
        comparisons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, label, folder] {
            if (current_directory_.isEmpty()) {
                QMessageBox::information(this, tr("没有几何结果"), tr("请先完成一次 C++ 几何生成。"));
                return;
            }
            const auto directory =
                QDir{current_directory_}.filePath(QStringLiteral("visualization/%1").arg(folder));
            if (!QDir{directory}.exists()) {
                QMessageBox::information(
                    this, label,
                    folder == QStringLiteral("tetrahedralization")
                        ? tr("当前结果没有四面体网格；请勾选 TetGen 后重新生成。")
                        : tr("当前结果没有对应的阶段制品。"));
                return;
            }
            auto* dialog = new GeometryComparisonDialog(label, current_directory_, this);
            dialog->show();
            log_->append(tr("已打开 %1 相机联动对比视口；主视口对象与显示状态保持不变。")
                             .arg(label));
        });
    }
    controls->addLayout(comparisons);
    auto* info_group = new QGroupBox(tr("已选对象网格信息"), controls_content);
    auto* info_layout = new QVBoxLayout(info_group);
    object_info_ = new QPlainTextEdit(tr("尚未选择可视对象。"), info_group);
    object_info_->setReadOnly(true);
    object_info_->setMaximumHeight(150);
    info_layout->addWidget(object_info_);
    controls->addWidget(info_group);
    controls->addStretch(1);

    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(controls_content);
    scroll->setMinimumWidth(430);
    splitter->addWidget(scroll);

    auto* right = new QSplitter(Qt::Vertical, splitter);
    viewport_ = new MeshViewport(right);
    right->addWidget(viewport_);
    log_ = new LogPanel(tr("设计与网格质量日志"), right);
    right->addWidget(log_);
    right->setSizes({660, 260});
    splitter->addWidget(right);
    splitter->setSizes({370, 1190});
    scene_status_ = new QLabel(tr("VTK 场景正在初始化"), controls_content);
    // MeshViewport already presents this status beside the scene controls.  Keep this
    // mirror as a signal target only; a parented widget without a layout position would
    // otherwise float over the first group-box title.
    scene_status_->hide();

    connect(lambda_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](const double value) { mu_->setMaximum(std::max(0.0, 1.0 - value)); });
    connect(random_phase_, &QCheckBox::toggled, this, [this](const bool random) {
        phase_x_->setEnabled(!random);
        phase_y_->setEnabled(!random);
        attempts_->setEnabled(random);
    });
    connect(simplify_, &QCheckBox::toggled, simplify_keep_percent_,
            &QDoubleSpinBox::setEnabled);
    connect(sharpen_, &QCheckBox::toggled, feature_angle_, &QDoubleSpinBox::setEnabled);
    feature_angle_->setEnabled(false);
    const auto update_sizing_controls =
        [this, remesh_form, target_edge_field, minimum_edge_field,
         maximum_edge_field](const int) {
            const auto adaptive = static_cast<domain::SurfaceSizingMode>(
                                      sizing_mode_->currentData().toInt()) ==
                                  domain::SurfaceSizingMode::curvature_adaptive;
            edge_percent_->setEnabled(!adaptive);
            minimum_edge_percent_->setEnabled(adaptive);
            maximum_edge_percent_->setEnabled(adaptive);
            remesh_form->setRowVisible(target_edge_field, !adaptive);
            remesh_form->setRowVisible(minimum_edge_field, adaptive);
            remesh_form->setRowVisible(maximum_edge_field, adaptive);
        };
    connect(sizing_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            update_sizing_controls);
    update_sizing_controls(sizing_mode_->currentIndex());
    const auto update_tet_controls = [this](const bool enabled) {
        for (auto* widget : std::array<QWidget*, 5>{tet_order_, tet_target_edge_,
                                                    tet_optimization_, tet_no_bisect_,
                                                    tet_quality_}) {
            widget->setEnabled(enabled);
        }
        tet_ratio_->setEnabled(enabled && tet_quality_->isChecked());
        tet_dihedral_->setEnabled(enabled && tet_quality_->isChecked());
    };
    connect(tetrahedralize_, &QCheckBox::toggled, this, update_tet_controls);
    connect(tet_quality_, &QCheckBox::toggled, this,
            [this, update_tet_controls](const bool) {
                update_tet_controls(tetrahedralize_->isChecked());
            });
    update_tet_controls(tetrahedralize_->isChecked());
    for (auto* input : {lambda_, mu_, kappa_, beta_, phase_x_, phase_y_, width_,
                        plate_thickness_, edge_percent_, surface_tolerance_,
                        minimum_edge_percent_, maximum_edge_percent_,
                        simplify_keep_percent_, feature_angle_, tet_dihedral_, tet_ratio_,
                        tet_target_edge_}) {
        connect(input, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this] { update_memory(); });
    }
    for (auto* input : {repeat_z_, resolution_, remesh_iterations_, repair_rounds_, attempts_,
                        tet_optimization_}) {
        connect(input, qOverload<int>(&QSpinBox::valueChanged), this,
                [this] { update_memory(); });
    }
    connect(save_sample_button, &QPushButton::clicked, this, [this] { save_sample(); });
    connect(start, &QPushButton::clicked, this, [this] {
        const auto config = design_config();
        const auto errors = config.validation_errors();
        if (!errors.empty()) {
            QMessageBox::warning(this, tr("设计配置无效"), validation_text(errors));
            return;
        }
        const auto source = static_cast<domain::DesignSource>(source_->currentData().toInt());
        auto sample_id = prepared_sample_id_;
        int sample_serial{};
        QString archive_directory;
        if (source == domain::DesignSource::existing && history_->currentIndex() >= 0) {
            sample_id = history_->currentData().toString();
            active_geometry_dataset_ = static_cast<domain::DatasetKind>(
                history_->currentData(Qt::UserRole + 1).toInt());
        } else if (source == domain::DesignSource::bayesian_optimization) {
            active_geometry_dataset_ = domain::DatasetKind::mbs;
        } else {
            active_geometry_dataset_ = domain::DatasetKind::demo;
        }
        if (sample_id.isEmpty()) {
            const auto samples = context_.datasets().list(active_geometry_dataset_);
            const auto allocation = allocate_sample(active_geometry_dataset_, config, samples);
            sample_id = allocation.id;
            sample_serial = allocation.serial;
            archive_directory = allocation.directory;
        } else {
            const auto samples = context_.datasets().list(active_geometry_dataset_);
            const auto id = sample_id.toStdString();
            const auto found = std::find_if(samples.begin(), samples.end(), [&id](const auto& sample) {
                return sample.id == id;
            });
            if (found != samples.end()) {
                sample_serial = found->serial;
                archive_directory = sample_artifact_directory(*found);
            }
        }
        reset_temporary_workspace();
        active_geometry_config_ = config;
        active_geometry_sample_id_ = sample_id;
        active_geometry_archive_directory_ = archive_directory.isEmpty()
                                                 ? sample_artifact_directory(sample_id)
                                                 : archive_directory;
        current_directory_ =
            QDir{temporary_workspace()}.filePath(QStringLiteral("geometry"));
        domain::Sample tracked;
        bool existing_record = false;
        const auto known_samples = context_.datasets().list(active_geometry_dataset_);
        const auto known_id = sample_id.toStdString();
        const auto known = std::find_if(known_samples.begin(), known_samples.end(),
                                        [&known_id](const auto& sample) {
                                            return sample.id == known_id;
                                        });
        if (known != known_samples.end()) {
            tracked = *known;
            existing_record = true;
        }
        tracked.id = sample_id.toStdString();
        tracked.project_id = "default";
        tracked.dataset = active_geometry_dataset_;
        tracked.serial = sample_serial;
        tracked.parameters = config.parameters;
        tracked.mesh = config.mesh;
        if (!existing_record) {
            tracked.source = source == domain::DesignSource::bayesian_optimization
                                 ? domain::DesignSource::bayesian_optimization
                                 : domain::DesignSource::manual;
        }
        tracked.status = progressive_geometry_status(tracked.status, QStringLiteral("geometry_running"))
                             .toStdString();
        tracked.artifact_directory = active_geometry_archive_directory_.toStdString();
        context_.datasets().save(tracked);
        if (existing_record) {
            log_->append(tr("自然键匹配到已有样本 %1：本次建模将复用该记录和目录，不分配新编号。")
                             .arg(sample_id));
        }
        const auto number = [](const double value) { return QString::number(value, 'g', 17); };
        const auto flag = [](const bool value) {
            return value ? QStringLiteral("true") : QStringLiteral("false");
        };
        QStringList arguments{QStringLiteral("geometry"),
                              QStringLiteral("--task-id"),
                              sample_id,
                              QStringLiteral("--run-id"),
                              sample_id,
                              QStringLiteral("--sample-id"),
                              sample_id,
                              QStringLiteral("--output-dir"),
                              current_directory_,
                              QStringLiteral("--lambda"),
                              number(config.parameters.lambda),
                              QStringLiteral("--mu"),
                              number(config.parameters.mu),
                              QStringLiteral("--kappa"),
                              number(config.parameters.kappa),
                              QStringLiteral("--beta"),
                              number(config.parameters.beta),
                              QStringLiteral("--phase-x"),
                              number(config.parameters.phase_x),
                              QStringLiteral("--phase-y"),
                              number(config.parameters.phase_y),
                              QStringLiteral("--random-phase"),
                              flag(config.random_phase),
                              QStringLiteral("--part-b-construction"),
                              QString::fromUtf8(domain::to_string(config.part_b_construction).data()),
                              QStringLiteral("--width"),
                              number(config.mesh.width),
                              QStringLiteral("--repeat-z"),
                              QString::number(config.mesh.repeat_z),
                              QStringLiteral("--plate-thickness"),
                              number(config.mesh.plate_thickness),
                              QStringLiteral("--resolution"),
                              QString::number(config.mesh.resolution),
                              QStringLiteral("--edge-percent"),
                              number(config.mesh.target_edge_percent),
                              QStringLiteral("--sizing-mode"),
                              QString::fromUtf8(domain::to_string(config.mesh.sizing_mode).data()),
                              QStringLiteral("--surface-tolerance-percent"),
                              number(config.mesh.surface_tolerance_percent),
                              QStringLiteral("--minimum-edge-percent"),
                              number(config.mesh.minimum_edge_percent),
                              QStringLiteral("--maximum-edge-percent"),
                              number(config.mesh.maximum_edge_percent),
                              QStringLiteral("--remesh-iterations"),
                              QString::number(config.mesh.remesh_iterations),
                              QStringLiteral("--feature-angle"),
                              number(config.mesh.feature_angle_degrees),
                              QStringLiteral("--sharpen"),
                              flag(config.mesh.sharpen),
                              QStringLiteral("--simplify"),
                              flag(config.mesh.simplify),
                              QStringLiteral("--simplify-keep-ratio"),
                              number(config.mesh.simplify_keep_ratio),
                              QStringLiteral("--repair-rounds"),
                              QString::number(config.mesh.repair_rounds),
                              QStringLiteral("--max-attempts"),
                              QString::number(config.mesh.max_attempts),
                              QStringLiteral("--tetrahedralize"),
                              flag(config.mesh.tetrahedralize),
                              QStringLiteral("--tet-order"),
                              QString::number(config.mesh.tetgen.order),
                              QStringLiteral("--tet-min-dihedral"),
                              number(config.mesh.tetgen.minimum_dihedral),
                              QStringLiteral("--tet-min-ratio"),
                              number(config.mesh.tetgen.minimum_ratio),
                              QStringLiteral("--tet-target-edge"),
                              number(config.mesh.tetgen.target_edge_length_mm),
                              QStringLiteral("--tet-optimization"),
                              QString::number(config.mesh.tetgen.optimization_level),
                              QStringLiteral("--tet-no-bisect"),
                              flag(config.mesh.tetgen.no_bisect),
                              QStringLiteral("--tet-quality"),
                              flag(config.mesh.tetgen.quality)};
        controller_.start_worker(arguments, tr("C++ TPMS 几何生成"));
    });
    connect(stop, &QPushButton::clicked, &controller_, &TaskController::stop);
    connect(load_existing, &QPushButton::clicked, this, [this] {
        const auto directory = QFileDialog::getExistingDirectory(this, tr("选择可视化网格目录"));
        if (directory.isEmpty()) {
            return;
        }
        const QDir chosen{directory};
        current_directory_ = chosen.dirName().compare(QStringLiteral("visualization"),
                                                       Qt::CaseInsensitive) == 0
                                 ? QFileInfo{directory}.dir().absolutePath()
                                 : directory;
        viewport_->clear_scene();
        const QDir selected{current_directory_};
        const auto visualization = selected.filePath(QStringLiteral("visualization"));
        const auto main_a = selected.filePath(QStringLiteral("tpms-tri-A.obj"));
        const auto main_b = selected.filePath(QStringLiteral("tpms-tri-B.obj"));
        auto count = QDir{visualization}.exists() ? viewport_->load_directory(visualization) : 0;
        if (count == 0 && QFileInfo::exists(main_a) && QFileInfo::exists(main_b)) {
            count = static_cast<int>(viewport_->load_file(main_a)) +
                    static_cast<int>(viewport_->load_file(main_b));
        }
        log_->append(tr("已从 %1 的 visualization 目录恢复 %2 个可视对象；B/R/T 阶段视图也可再次打开。")
                         .arg(current_directory_).arg(count));
    });
    connect(open_current, &QPushButton::clicked, this, [this] {
        if (current_directory_.isEmpty()) {
            QMessageBox::information(this, tr("当前目录"), tr("请先加载已有模型目录。"));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(current_directory_));
    });
    connect(contact_risk, &QPushButton::clicked, this, [this] {
        const QDir directory{current_directory_};
        if (current_directory_.isEmpty() ||
            !directory.exists(QStringLiteral("tpms-tet-A.inp")) ||
            !directory.exists(QStringLiteral("tpms-tet-B.inp"))) {
            QMessageBox::information(
                this, tr("接触风险检查"),
                tr("请先生成或载入同时包含 tpms-tet-A.inp 与 tpms-tet-B.inp 的四面体网格目录。"));
            return;
        }
        const auto task_id = QStringLiteral("contact-%1").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
        controller_.start_worker(
            {QStringLiteral("contact-risk"), QStringLiteral("--task-id"), task_id,
             QStringLiteral("--mesh-dir"), current_directory_},
            tr("C++ 接触风险分析"));
    });
    connect(generate, &QPushButton::clicked, this, [this] {
        const auto task_id = QStringLiteral("bo-%1").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
        controller_.start_worker(
            {QStringLiteral("optimize"), QStringLiteral("--task-id"), task_id,
             QStringLiteral("--database"), path_text(context_.database_path()),
             QStringLiteral("--acquisition"), acquisition_->currentText(),
             QStringLiteral("--initial-points"), QStringLiteral("10"),
             QStringLiteral("--random-seed"), QStringLiteral("0"),
             QStringLiteral("--candidate-pool"), QStringLiteral("16"),
             QStringLiteral("--kappa"), QStringLiteral("1.96"), QStringLiteral("--xi"),
             QStringLiteral("0.01")},
            tr("C++ 贝叶斯候选生成"));
    });
    connect(load, &QPushButton::clicked, this, [this] {
        const auto pending = context_.optimization().load_pending_json();
        if (!pending.has_value()) {
            QMessageBox::information(this, tr("BO 候选"), tr("当前没有待建模候选。"));
            return;
        }
        const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(*pending)).object();
        const auto values = object.value(QStringLiteral("params")).toArray();
        if (values.size() == 4) {
            lambda_->setValue(values[0].toDouble());
            mu_->setValue(values[1].toDouble());
            kappa_->setValue(values[2].toDouble());
            beta_->setValue(values[3].toDouble());
            log_->append(tr("已载入 SQLite 中的待建模 BO 候选。"));
        } else {
            stage_notice(7, tr("BO 候选协议解析"));
        }
    });
    connect(history_refresh, &QPushButton::clicked, this, [this] { refresh_history(); });
    connect(history_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { load_history(); });
    connect(source_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const auto historical = static_cast<domain::DesignSource>(source_->currentData().toInt()) ==
                                domain::DesignSource::existing;
        history_->setEnabled(historical);
        if (historical) {
            load_history();
        }
    });
    connect(viewport_, &MeshViewport::message, log_, &LogPanel::append);
    connect(viewport_, &MeshViewport::selection_info_changed, object_info_,
            &QPlainTextEdit::setPlainText);
    connect(viewport_, &MeshViewport::scene_changed, this,
            [this](const QString& text) { scene_status_->setText(text); });
    connect(&controller_, &TaskController::log_received, log_, &LogPanel::append);
    connect(&controller_, &TaskController::busy_changed, this, [start, stop](const bool busy) {
        start->setEnabled(!busy);
        stop->setEnabled(busy);
    });
    connect(&controller_, &TaskController::finished, this,
            [this](const bool success, const QString& task_label) {
                if (task_label == tr("C++ 贝叶斯候选生成")) {
                    if (success) {
                        const auto pending = context_.optimization().load_pending_json();
                        pending_->setText(pending_summary(pending));
                        log_->append(tr("✓ 新候选已保存到 SQLite，可点击“载入候选”。"));
                    }
                    return;
                }
                if (task_label != tr("C++ TPMS 几何生成")) {
                    return;
                }
                if (!success) {
                    log_->append(tr("几何生成失败；未发布不完整制品。"));
                    if (!active_geometry_sample_id_.isEmpty()) {
                        for (auto dataset : {domain::DatasetKind::mbs, domain::DatasetKind::demo}) {
                            auto samples = context_.datasets().list(dataset);
                            const auto id = active_geometry_sample_id_.toStdString();
                            const auto found = std::find_if(samples.begin(), samples.end(),
                                                            [&id](const auto& sample) {
                                                                return sample.id == id;
                            });
                            if (found != samples.end()) {
                                found->status = progressive_geometry_status(
                                                    found->status,
                                                    QStringLiteral("geometry_failed"))
                                                    .toStdString();
                                context_.datasets().save(*found);
                            }
                        }
                    }
                    current_directory_.clear();
                    active_geometry_sample_id_.clear();
                    active_geometry_archive_directory_.clear();
                    return;
                }
                try {
                    archive_geometry_files(current_directory_, active_geometry_archive_directory_);
                    current_directory_ = active_geometry_archive_directory_;
                } catch (const std::exception& error) {
                    log_->append(tr("几何已生成，但归档到样本根目录失败：%1")
                                     .arg(QString::fromUtf8(error.what())));
                    auto samples = context_.datasets().list(active_geometry_dataset_);
                    const auto id = active_geometry_sample_id_.toStdString();
                    const auto found = std::find_if(samples.begin(), samples.end(),
                                                    [&id](const auto& sample) {
                                                        return sample.id == id;
                                                    });
                    if (found != samples.end()) {
                        found->status = "geometry_archive_failed";
                        context_.datasets().save(*found);
                    }
                    return;
                }
                viewport_->clear_scene();
                auto count = viewport_->load_directory(
                    QDir{current_directory_}.filePath(QStringLiteral("visualization")));
                if (count == 0) {
                    count = static_cast<int>(viewport_->load_file(
                                QDir{current_directory_}.filePath(
                                    QStringLiteral("tpms-tri-A.obj")))) +
                            static_cast<int>(viewport_->load_file(
                                QDir{current_directory_}.filePath(
                                    QStringLiteral("tpms-tri-B.obj"))));
                }
                log_->append(tr("✓ C++ 几何已在 %1 生成，并将 INP/OBJ/JSON 归档到 %2；载入 %3 个表面对象。")
                                 .arg(current_directory_, active_geometry_archive_directory_)
                                 .arg(count));
                const auto completed_id = active_geometry_sample_id_.toStdString();
                auto samples = context_.datasets().list(active_geometry_dataset_);
                const auto found = std::find_if(samples.begin(), samples.end(),
                                                [&completed_id](const auto& sample) {
                                                    return sample.id == completed_id;
                                                });
                if (found != samples.end()) {
                    QFile metadata_file{QDir{current_directory_}.filePath(
                        QStringLiteral("mesh_metadata.json"))};
                    if (metadata_file.open(QIODevice::ReadOnly)) {
                        const auto metadata =
                            QJsonDocument::fromJson(metadata_file.readAll()).object();
                        found->parameters.phase_x = metadata.value(QStringLiteral("rnd_x"))
                                                        .toDouble(found->parameters.phase_x);
                        found->parameters.phase_y = metadata.value(QStringLiteral("rnd_y"))
                                                        .toDouble(found->parameters.phase_y);
                    }
                    found->status = progressive_geometry_status(
                                        found->status, QStringLiteral("geometry_ready"))
                                        .toStdString();
                    context_.datasets().save(*found);
                }
                prepared_sample_id_.clear();
                active_geometry_sample_id_.clear();
                active_geometry_archive_directory_.clear();
                refresh_history();
            });
    refresh_history();
    update_memory();
}

MeshViewport* DesignPage::viewport() const noexcept { return viewport_; }

domain::DesignConfig DesignPage::design_config() const {
    domain::DesignConfig config;
    config.parameters = {.lambda = lambda_->value(),
                         .mu = mu_->value(),
                         .kappa = kappa_->value(),
                         .beta = beta_->value(),
                         .phase_x = phase_x_->value(),
                         .phase_y = phase_y_->value()};
    config.mesh.width = width_->value();
    config.mesh.repeat_z = repeat_z_->value();
    config.mesh.plate_thickness = plate_thickness_->value();
    config.mesh.resolution = resolution_->value();
    config.mesh.target_edge_percent = edge_percent_->value();
    config.mesh.sizing_mode = static_cast<domain::SurfaceSizingMode>(
        sizing_mode_->currentData().toInt());
    config.mesh.surface_tolerance_percent = surface_tolerance_->value();
    config.mesh.minimum_edge_percent = minimum_edge_percent_->value();
    config.mesh.maximum_edge_percent = maximum_edge_percent_->value();
    config.mesh.remesh_iterations = remesh_iterations_->value();
    config.mesh.feature_angle_degrees = feature_angle_->value();
    config.mesh.sharpen = sharpen_->isChecked();
    config.mesh.simplify = simplify_->isChecked();
    config.mesh.simplify_keep_ratio = simplify_keep_percent_->value() / 100.0;
    config.mesh.repair_rounds = repair_rounds_->value();
    config.mesh.max_attempts = attempts_->value();
    config.mesh.tetrahedralize = tetrahedralize_->isChecked();
    config.mesh.tetgen.order = tet_order_->currentData().toInt();
    config.mesh.tetgen.minimum_dihedral = tet_dihedral_->value();
    config.mesh.tetgen.minimum_ratio = tet_ratio_->value();
    config.mesh.tetgen.target_edge_length_mm = tet_target_edge_->value();
    config.mesh.tetgen.optimization_level = tet_optimization_->value();
    config.mesh.tetgen.no_bisect = tet_no_bisect_->isChecked();
    config.mesh.tetgen.quality = tet_quality_->isChecked();
    config.source = static_cast<domain::DesignSource>(source_->currentData().toInt());
    config.random_phase = random_phase_->isChecked();
    config.part_b_construction = static_cast<domain::PartBConstruction>(
        part_b_construction_->currentData().toInt());
    return config;
}

void DesignPage::update_preview() {
    const auto config = design_config();
    const auto errors = config.validation_errors();
    if (!errors.empty()) {
        QMessageBox::warning(this, tr("参数无效"), validation_text(errors));
        return;
    }
    viewport_->show_demo(config.parameters);
    log_->append(tr("C++/VTK 快速预览已更新；正式结构以几何内核生成结果为准。"));
}

void DesignPage::update_memory() {
    const auto mesh = design_config().mesh;
    memory_->setText(tr("约 %1 百万采样点 · %2 GiB 峰值内存")
                         .arg(static_cast<double>(mesh.estimated_grid_points()) / 1.0e6, 0, 'f', 2)
                         .arg(static_cast<double>(mesh.estimated_memory_bytes()) /
                                  (1024.0 * 1024.0 * 1024.0),
                              0, 'f', 2));
}

void DesignPage::save_sample() {
    auto config = design_config();
    const auto errors = config.validation_errors();
    if (!errors.empty()) {
        QMessageBox::warning(this, tr("参数无效"), validation_text(errors));
        return;
    }
    const auto source = config.source;
    const auto is_bo = source == domain::DesignSource::bayesian_optimization;
    domain::Sample sample;
    const auto dataset = is_bo ? domain::DatasetKind::mbs : domain::DatasetKind::demo;
    const auto samples = context_.datasets().list(dataset);
    const auto allocation = allocate_sample(dataset, config, samples);
    if (allocation.reused) {
        const auto id = allocation.id.toStdString();
        const auto existing = std::find_if(samples.begin(), samples.end(), [&id](const auto& value) {
            return value.id == id;
        });
        if (existing != samples.end()) {
            sample = *existing;
        }
    }
    sample.id = allocation.id.toStdString();
    sample.project_id = "default";
    sample.dataset = dataset;
    sample.serial = allocation.serial;
    sample.parameters = config.parameters;
    sample.mesh = config.mesh;
    if (!allocation.reused) {
        sample.source = is_bo ? domain::DesignSource::bayesian_optimization
                              : domain::DesignSource::manual;
        sample.status = is_bo ? "pending_geometry" : "preview_saved";
    }
    sample.artifact_directory = allocation.directory.toStdString();
    context_.datasets().save(sample);
    prepared_sample_id_ = QString::fromStdString(sample.id);
    if (is_bo) {
        context_.optimization().clear_pending();
    }
    log_->append(allocation.reused
                     ? tr("六参数自然键已存在，复用 SQLite 样本：%1（未新增行）")
                           .arg(QString::fromStdString(sample.id))
                     : tr("SQLite %1 样本已保存：%2")
                           .arg(is_bo ? QStringLiteral("MBS/BO") : QStringLiteral("Demo"),
                                QString::fromStdString(sample.id)));
    refresh_history();
}

void DesignPage::refresh_history() {
    const auto current = history_->currentData();
    history_->blockSignals(true);
    history_->clear();
    for (const auto dataset : {domain::DatasetKind::mbs, domain::DatasetKind::demo}) {
        for (const auto& sample : context_.datasets().list(dataset)) {
            history_->addItem(
                QStringLiteral("[%1] %2")
                    .arg(QString::fromUtf8(domain::to_string(dataset).data()), sample_label(sample)),
                QString::fromStdString(sample.id));
            history_->setItemData(history_->count() - 1, static_cast<int>(dataset), Qt::UserRole + 1);
        }
    }
    const auto index = history_->findData(current);
    if (index >= 0) {
        history_->setCurrentIndex(index);
    }
    history_->blockSignals(false);
    history_->setEnabled(static_cast<domain::DesignSource>(source_->currentData().toInt()) ==
                         domain::DesignSource::existing);
    const auto pending = context_.optimization().load_pending_json();
    pending_->setText(pending_summary(pending));
}

void DesignPage::load_history() {
    if (static_cast<domain::DesignSource>(source_->currentData().toInt()) !=
            domain::DesignSource::existing ||
        history_->currentIndex() < 0) {
        return;
    }
    const auto dataset = static_cast<domain::DatasetKind>(history_->currentData(Qt::UserRole + 1).toInt());
    const auto id = history_->currentData().toString().toStdString();
    const auto samples = context_.datasets().list(dataset);
    const auto found = std::find_if(samples.begin(), samples.end(), [&id](const auto& sample) {
        return sample.id == id;
    });
    if (found == samples.end()) {
        return;
    }
    lambda_->setValue(found->parameters.lambda);
    mu_->setValue(found->parameters.mu);
    kappa_->setValue(found->parameters.kappa);
    beta_->setValue(found->parameters.beta);
    phase_x_->setValue(found->parameters.phase_x);
    phase_y_->setValue(found->parameters.phase_y);
    random_phase_->setChecked(false);
    width_->setValue(found->mesh.width);
    repeat_z_->setValue(found->mesh.repeat_z);
    plate_thickness_->setValue(found->mesh.plate_thickness);
    resolution_->setValue(found->mesh.resolution);
    edge_percent_->setValue(found->mesh.target_edge_percent);
    sizing_mode_->setCurrentIndex(
        sizing_mode_->findData(static_cast<int>(found->mesh.sizing_mode)));
    surface_tolerance_->setValue(found->mesh.surface_tolerance_percent);
    minimum_edge_percent_->setValue(found->mesh.minimum_edge_percent);
    maximum_edge_percent_->setValue(found->mesh.maximum_edge_percent);
    remesh_iterations_->setValue(found->mesh.remesh_iterations);
    feature_angle_->setValue(found->mesh.feature_angle_degrees);
    sharpen_->setChecked(found->mesh.sharpen);
    simplify_->setChecked(found->mesh.simplify);
    simplify_keep_percent_->setValue(found->mesh.simplify_keep_ratio * 100.0);
    repair_rounds_->setValue(found->mesh.repair_rounds);
    attempts_->setValue(found->mesh.max_attempts);
    tetrahedralize_->setChecked(found->mesh.tetrahedralize);
    tet_order_->setCurrentIndex(tet_order_->findData(found->mesh.tetgen.order));
    tet_dihedral_->setValue(found->mesh.tetgen.minimum_dihedral);
    tet_ratio_->setValue(found->mesh.tetgen.minimum_ratio);
    tet_target_edge_->setValue(found->mesh.tetgen.target_edge_length_mm);
    tet_optimization_->setValue(found->mesh.tetgen.optimization_level);
    tet_no_bisect_->setChecked(found->mesh.tetgen.no_bisect);
    tet_quality_->setChecked(found->mesh.tetgen.quality);
    log_->append(tr("已载入历史样本 %1 的全部设计参数。").arg(QString::fromStdString(id)));
}

void DesignPage::stage_notice(const int stage, const QString& feature) {
    QMessageBox::information(
        this, tr("功能入口已恢复"),
        tr("“%1”按钮和参数入口已按 3.0 恢复。\n\n真正执行需要阶段 %2；当前不会调用 3.0 源码，也不会伪造结果。")
            .arg(feature)
            .arg(stage));
}

SimulationPage::SimulationPage(ApplicationContext& context, TaskController& controller,
                               QWidget* parent)
    : QWidget(parent), context_(context), controller_(controller) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);
    auto* controls_content = new QWidget(splitter);
    auto* controls = new QVBoxLayout(controls_content);

    auto* source = new QGroupBox(tr("网格输入"), controls_content);
    auto* source_form = new QFormLayout(source);
    input_mode_ = new NoWheelComboBox(source);
    input_mode_->addItem(tr("数据库已建样本"), QStringLiteral("sample"));
    input_mode_->addItem(tr("外部网格目录"), QStringLiteral("external"));
    samples_ = new NoWheelComboBox(source);
    auto* refresh_button = new QPushButton(tr("刷新"), source);
    auto* sample_row = new QHBoxLayout;
    sample_row->addWidget(samples_, 1);
    sample_row->addWidget(refresh_button);
    external_directory_ = new QLineEdit(source);
    auto* browse = new QPushButton(tr("浏览"), source);
    auto* external_row = new QHBoxLayout;
    external_row->addWidget(external_directory_, 1);
    external_row->addWidget(browse);
    backend_ = new NoWheelComboBox(source);
    backend_->addItem(tr("TetGen 体网格（tet.inp）"), static_cast<int>(domain::SimulationBackend::tetgen));
    backend_->addItem(tr("Abaqus 内部四面体化（tri.inp）"), static_cast<int>(domain::SimulationBackend::abaqus));
    capabilities_ = new QLabel(tr("Tri: 待读取  Tet: 待读取  ODB: 待读取"), source);
    capabilities_->setWordWrap(true);
    source_form->addRow(tr("输入来源"), input_mode_);
    source_form->addRow(tr("样本"), sample_row);
    source_form->addRow(tr("外部目录"), external_row);
    source_form->addRow(tr("仿真网格方式"), backend_);
    source_form->addRow(tr("可用文件"), capabilities_);
    controls->addWidget(source);

    abaqus_mesh_group_ = new QGroupBox(tr("Abaqus 内部四面体化控制"), controls_content);
    auto* abaqus_mesh_form = new QFormLayout(abaqus_mesh_group_);
    target_size_enabled_ = new QCheckBox(tr("指定内部目标单元尺寸"), abaqus_mesh_group_);
    target_size_enabled_->setChecked(true);
    target_size_ = double_input(0.6, 0.001, 1000.0, 4, 0.05);
    abaqus_mesh_form->addRow(target_size_enabled_);
    abaqus_mesh_form->addRow(tr("内部目标尺寸 / mm"), target_size_);
    auto* mesh_note = new QLabel(
        tr("三角形孤立网格转换四面体时指定全局目标尺寸；接触面密度仍由设计网格决定。"),
        abaqus_mesh_group_);
    mesh_note->setWordWrap(true);
    mesh_note->setObjectName(QStringLiteral("mutedLabel"));
    abaqus_mesh_form->addRow(mesh_note);
    controls->addWidget(abaqus_mesh_group_);

    auto* common = new QGroupBox(tr("常用仿真参数"), controls_content);
    auto* common_form = new QFormLayout(common);
    QSettings settings;
    abaqus_command_ = new QLineEdit(settings.value(QStringLiteral("abaqus_command"),
                                                   QStringLiteral("abaqus")).toString(), common);
    width_ = double_input(10.0, 0.001, 1000.0, 4, 0.5);
    repeat_z_ = integer_input(3, 1, 100);
    plate_thickness_ = double_input(1.0, 0.001, 1000.0, 4, 0.1);
    geometry_source_ = new QLabel(tr("尺寸来源：待读取"), common);
    geometry_source_->setWordWrap(true);
    step_time_ = double_input(1.0, 0.0001, 1.0e6, 4, 0.1);
    displacement_ = double_input(1.0, 0.0, 30.0, 4, 0.1);
    cpus_ = integer_input(10, 1, 16);
    memory_percent_ = integer_input(90, 1, 100, 5);
    common_form->addRow(tr("Abaqus 命令"), abaqus_command_);
    common_form->addRow(tr("wth / mm"), width_);
    common_form->addRow(tr("rep_z"), repeat_z_);
    common_form->addRow(tr("thk_p / mm"), plate_thickness_);
    common_form->addRow(tr("尺寸状态"), geometry_source_);
    common_form->addRow(tr("分析步时长"), step_time_);
    common_form->addRow(tr("拉伸位移 / mm"), displacement_);
    common_form->addRow(tr("CPU 核数"), cpus_);
    common_form->addRow(tr("内存比例 %"), memory_percent_);
    controls->addWidget(common);

    advanced_toggle_ = new QPushButton(tr("▶ 展开高级参数"), controls_content);
    advanced_toggle_->setCheckable(true);
    advanced_toggle_->setObjectName(QStringLiteral("advancedParametersToggle"));
    controls->addWidget(advanced_toggle_);
    advanced_ = new QGroupBox(tr("高级参数"), controls_content);
    advanced_->setVisible(false);
    auto* advanced_layout = new QVBoxLayout(advanced_);
    auto* advanced_tabs = new QTabWidget(advanced_);
    auto* material_tab = new QWidget(advanced_tabs);
    auto* material_layout = new QVBoxLayout(material_tab);
    auto* material_slots = new QTabWidget(material_tab);
    material_slots->setObjectName(QStringLiteral("materialSlotTabs"));
    material_a_ = new MaterialEditor(context_, QStringLiteral("Bambu PLA-CF"), material_slots);
    material_b_ = new MaterialEditor(context_, QStringLiteral("Bambu PETG"), material_slots);
    material_slots->addTab(material_a_, tr("材料 A"));
    material_slots->addTab(material_b_, tr("材料 B"));
    material_layout->addWidget(material_slots);
    advanced_tabs->addTab(material_tab, tr("材料"));

    auto* solver_tab = new QWidget(advanced_tabs);
    auto* solver_form = new QFormLayout(solver_tab);
    stabilization_ = double_input(0.0002, 0.0, 1.0, 7, 0.0001);
    damping_ = double_input(0.05, 0.0, 1.0, 5, 0.01);
    maximum_increments_ = integer_input(500, 1, 100000);
    initial_increment_ = double_input(0.001, 1.0e-12, 1.0e6, 9, 0.001);
    minimum_increment_ = double_input(1.0e-5, 1.0e-12, 1.0e6, 9, 1.0e-5);
    maximum_increment_ = double_input(0.1, 1.0e-12, 1.0e6, 9, 0.01);
    solver_form->addRow(tr("稳定化幅值"), stabilization_);
    solver_form->addRow(tr("自适应阻尼"), damping_);
    solver_form->addRow(tr("最大增量步数"), maximum_increments_);
    solver_form->addRow(tr("初始增量"), initial_increment_);
    solver_form->addRow(tr("最小增量"), minimum_increment_);
    solver_form->addRow(tr("最大增量"), maximum_increment_);
    advanced_tabs->addTab(solver_tab, tr("求解"));

    auto* contact_tab = new QWidget(advanced_tabs);
    auto* contact_form = new QFormLayout(contact_tab);
    contact_ = new NoWheelComboBox(contact_tab);
    contact_->addItem(tr("无摩擦（FRICTIONLESS）"), static_cast<int>(domain::ContactFormulation::frictionless));
    contact_->addItem(tr("罚函数（PENALTY）"), static_cast<int>(domain::ContactFormulation::penalty));
    friction_ = double_input(0.0, 0.0, 10.0, 4, 0.05);
    sliding_ = new NoWheelComboBox(contact_tab);
    sliding_->addItem(tr("小滑移（SMALL）"), static_cast<int>(domain::SlidingFormulation::small));
    sliding_->addItem(tr("有限滑移（FINITE）"), static_cast<int>(domain::SlidingFormulation::finite));
    adjustment_ = new NoWheelComboBox(contact_tab);
    adjustment_->addItem(tr("仅调整初始过闭合（OVERCLOSED）"), static_cast<int>(domain::ContactAdjustment::overclosed));
    adjustment_->addItem(tr("不调整（NONE）"), static_cast<int>(domain::ContactAdjustment::none));
    adjustment_->addItem(tr("按容差调整（TOLERANCE）"), static_cast<int>(domain::ContactAdjustment::tolerance));
    adjustment_tolerance_ = double_input(0.01, 1.0e-12, 1000.0, 7, 0.001);
    vertex_tolerance_ = double_input(0.0001, 1.0e-8, 10.0, 7, 0.0001);
    edge_tolerance_ = double_input(0.001, 1.0e-8, 10.0, 7, 0.001);
    edge_tolerance_->setEnabled(false);
    surface_tolerance_ = double_input(0.01, 1.0e-8, 10.0, 7, 0.001);
    angle_tolerance_ = double_input(0.1, 1.0e-8, 180.0, 5, 0.01);
    angle_tolerance_->setEnabled(false);
    contact_form->addRow(tr("切向形式"), contact_);
    contact_form->addRow(tr("摩擦系数"), friction_);
    contact_form->addRow(tr("滑移形式"), sliding_);
    contact_form->addRow(tr("调整方法"), adjustment_);
    contact_form->addRow(tr("调整容差 / mm"), adjustment_tolerance_);
    contact_form->addRow(tr("顶点容差 / mm"), vertex_tolerance_);
    contact_form->addRow(tr("边容差 / mm"), edge_tolerance_);
    contact_form->addRow(tr("表面容差 / mm"), surface_tolerance_);
    contact_form->addRow(tr("角度容差 / rad"), angle_tolerance_);
    advanced_tabs->addTab(contact_tab, tr("接触与容差"));
    advanced_layout->addWidget(advanced_tabs);
    controls->addWidget(advanced_);

    auto* actions = new QHBoxLayout;
    auto* start = new QPushButton(tr("开始仿真"), controls_content);
    start->setObjectName(QStringLiteral("primaryButton"));
    auto* stop = new QPushButton(tr("终止作业"), controls_content);
    stop->setEnabled(false);
    actions->addWidget(start);
    actions->addWidget(stop);
    controls->addLayout(actions);
    controls->addStretch(1);

    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(controls_content);
    scroll->setMinimumWidth(390);
    splitter->addWidget(scroll);
    auto* right = new QSplitter(Qt::Vertical, splitter);
    player_ = new FramePlayer(right);
    right->addWidget(player_);
    auto* lower = new QWidget(right);
    auto* lower_layout = new QVBoxLayout(lower);
    lower_layout->setContentsMargins(0, 0, 0, 0);
    progress_ = new QProgressBar(lower);
    progress_->setRange(0, 100);
    progress_->setFormat(tr("Abaqus 求解进度 %p%"));
    log_ = new LogPanel(tr("Abaqus 实时日志"), lower);
    lower_layout->addWidget(progress_);
    lower_layout->addWidget(log_);
    right->addWidget(lower);
    right->setSizes({580, 310});
    splitter->addWidget(right);
    splitter->setSizes({410, 1150});

    connect(refresh_button, &QPushButton::clicked, this, [this] { refresh(); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto directory = QFileDialog::getExistingDirectory(this, tr("选择网格目录"));
        if (!directory.isEmpty()) {
            external_directory_->setText(directory);
            update_input_state();
        }
    });
    connect(input_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { update_input_state(); });
    connect(samples_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { update_input_state(); });
    connect(backend_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        abaqus_mesh_group_->setVisible(static_cast<domain::SimulationBackend>(
                                           backend_->currentData().toInt()) ==
                                       domain::SimulationBackend::abaqus);
    });
    connect(target_size_enabled_, &QCheckBox::toggled, target_size_, &QWidget::setEnabled);
    connect(advanced_toggle_, &QPushButton::toggled, this, [this](const bool expanded) {
        advanced_->setVisible(expanded);
        advanced_toggle_->setText(expanded ? tr("▼ 折叠高级参数") : tr("▶ 展开高级参数"));
    });
    connect(contact_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        friction_->setVisible(static_cast<domain::ContactFormulation>(
                                  contact_->currentData().toInt()) ==
                              domain::ContactFormulation::penalty);
    });
    connect(adjustment_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        adjustment_tolerance_->setVisible(static_cast<domain::ContactAdjustment>(
                                              adjustment_->currentData().toInt()) ==
                                          domain::ContactAdjustment::tolerance);
    });
    connect(start, &QPushButton::clicked, this, [this] { start_simulation(); });
    connect(stop, &QPushButton::clicked, this, [this] {
        if (!active_work_directory_.isEmpty()) {
            const auto config = simulation_config();
            QProcess::startDetached(QString::fromStdString(config.abaqus_command),
                                    {QStringLiteral("terminate"),
                                     QStringLiteral("job=%1").arg(
                                         QString::fromStdString(config.job_name))},
                                    active_work_directory_);
            log_->append(tr("已请求 Abaqus 优雅终止作业；随后关闭 Worker。"));
        }
        controller_.stop();
    });
    connect(&controller_, &TaskController::log_received, log_, &LogPanel::append);
    connect(&controller_, &TaskController::event_received, this,
            [this](const QString&, const QString&, const QString&, const int progress) {
                if (progress >= 0) {
                    progress_->setValue(progress);
                }
            });
    connect(&controller_, &TaskController::busy_changed, this, [start, stop](const bool busy) {
        start->setEnabled(!busy);
        stop->setEnabled(busy);
    });
    connect(&controller_, &TaskController::finished, this,
            [this](const bool success, const QString& label) {
                try {
                    if (label == tr("Abaqus 求解")) {
                        if (!success) {
                            log_->append(tr("Abaqus 求解失败或被取消；不会启动后处理。"));
                            finish_simulation_lifecycle(domain::TaskStatus::failed,
                                                        tr("Abaqus 求解失败或被取消"));
                            return;
                        }
                        active_odb_path_ = controller_.artifact_uri(QStringLiteral("odb_path"));
                        start_postprocess();
                    } else if (label == tr("Abaqus ODB 后处理")) {
                        if (!success) {
                            log_->append(tr("ODB 后处理失败；原始 ODB 保持不变。"));
                            finish_simulation_lifecycle(domain::TaskStatus::failed,
                                                        tr("ODB 后处理失败"));
                            return;
                        }
                        active_result_path_ =
                            controller_.artifact_uri(QStringLiteral("result_path"));
                        start_animation_export();
                    } else if (label == tr("Abaqus 云图动画导出")) {
                        if (!success) {
                            log_->append(tr("动画导出失败；ODB 与结构化后处理结果仍然有效。"));
                            finish_simulation_workflow();
                            return;
                        }
                        active_manifest_path_ =
                            controller_.artifact_uri(QStringLiteral("animation_manifest"));
                        finish_simulation_workflow();
                    }
                } catch (const std::exception& error) {
                    log_->append(tr("Abaqus 工作流停止：%1").arg(QString::fromUtf8(error.what())));
                    try {
                        finish_simulation_lifecycle(domain::TaskStatus::failed,
                                                    QString::fromUtf8(error.what()));
                    } catch (const std::exception& lifecycle_error) {
                        log_->append(tr("SQLite 运行状态更新失败：%1")
                                         .arg(QString::fromUtf8(lifecycle_error.what())));
                    }
                }
            });
    friction_->setVisible(false);
    adjustment_tolerance_->setVisible(false);
    refresh();
}

void SimulationPage::refresh() {
    const auto current = samples_->currentData();
    samples_->blockSignals(true);
    samples_->clear();
    for (const auto dataset : {domain::DatasetKind::mbs, domain::DatasetKind::demo}) {
        for (const auto& sample : context_.datasets().list(dataset)) {
            samples_->addItem(
                QStringLiteral("%1 | %2")
                    .arg(QString::fromUtf8(domain::to_string(dataset).data()), sample_label(sample)),
                QString::fromStdString(sample.id));
            samples_->setItemData(samples_->count() - 1, static_cast<int>(dataset), Qt::UserRole + 1);
        }
    }
    const auto index = samples_->findData(current);
    if (index >= 0) {
        samples_->setCurrentIndex(index);
    }
    samples_->blockSignals(false);
    material_a_->reload_library();
    material_b_->reload_library();
    update_input_state();
}

void SimulationPage::update_input_state() {
    const auto external = input_mode_->currentData().toString() == QStringLiteral("external");
    samples_->setEnabled(!external);
    external_directory_->setEnabled(external);
    if (external) {
        const QDir directory{external_directory_->text()};
        const auto tri = directory.exists(QStringLiteral("tpms-tri-A.inp")) &&
                         directory.exists(QStringLiteral("tpms-tri-B.inp"));
        const auto tet = directory.exists(QStringLiteral("tpms-tet-A.inp")) &&
                         directory.exists(QStringLiteral("tpms-tet-B.inp"));
        const auto odb = directory.exists(QStringLiteral("job-intlck-tpms.odb"));
        capabilities_->setText(QStringLiteral("Tri:%1  Tet:%2  ODB:%3")
                                   .arg(tri ? QStringLiteral("✓") : QStringLiteral("×"),
                                        tet ? QStringLiteral("✓") : QStringLiteral("×"),
                                        odb ? QStringLiteral("✓") : QStringLiteral("×")));
        geometry_source_->setText(tr("外部网格尺寸：请手动确认；目录能力已实时检查"));
    } else {
        const auto dataset = static_cast<domain::DatasetKind>(samples_->currentData(Qt::UserRole + 1).toInt());
        const auto id = samples_->currentData().toString().toStdString();
        const auto values = context_.datasets().list(dataset);
        const auto found = std::find_if(values.begin(), values.end(), [&id](const auto& sample) {
            return sample.id == id;
        });
        if (found != values.end()) {
            width_->setValue(found->mesh.width);
            repeat_z_->setValue(found->mesh.repeat_z);
            plate_thickness_->setValue(found->mesh.plate_thickness);
            geometry_source_->setText(tr("尺寸来源：SQLite 样本设计参数"));
            const QDir artifacts{sample_artifact_directory(*found)};
            const auto tri = artifacts.exists(QStringLiteral("tpms-tri-A.inp")) &&
                             artifacts.exists(QStringLiteral("tpms-tri-B.inp"));
            const auto tet = artifacts.exists(QStringLiteral("tpms-tet-A.inp")) &&
                             artifacts.exists(QStringLiteral("tpms-tet-B.inp"));
            capabilities_->setText(QStringLiteral("目录：%1\nTri:%2  Tet:%3")
                                       .arg(artifacts.absolutePath(), tri ? QStringLiteral("✓")
                                                                         : QStringLiteral("×"),
                                            tet ? QStringLiteral("✓") : QStringLiteral("×")));
        }
    }
    displacement_->setMaximum(std::max(0.0, width_->value() * repeat_z_->value()));
    abaqus_mesh_group_->setVisible(
        static_cast<domain::SimulationBackend>(backend_->currentData().toInt()) ==
        domain::SimulationBackend::abaqus);
}

domain::SimulationConfig SimulationPage::simulation_config() const {
    domain::SimulationConfig config;
    const auto external = input_mode_->currentData().toString() == QStringLiteral("external");
    const auto selected_id = samples_->currentData().toString();
    config.sample_id = (active_sample_id_.isEmpty() ? selected_id : active_sample_id_).toStdString();
    config.mesh_directory =
        (active_mesh_directory_.isEmpty()
             ? (external ? external_directory_->text()
                         : known_sample_artifact_directory(context_, selected_id))
             : active_mesh_directory_)
            .toStdString();
    config.work_directory =
        (active_work_directory_.isEmpty()
             ? QDir{artifact_root()}.filePath(QStringLiteral("pending"))
             : active_work_directory_)
            .toStdString();
    config.backend = static_cast<domain::SimulationBackend>(backend_->currentData().toInt());
    config.abaqus_command = abaqus_command_->text().trimmed().toStdString();
    config.width = width_->value();
    config.repeat_z = repeat_z_->value();
    config.plate_thickness = plate_thickness_->value();
    config.tensile_displacement = displacement_->value();
    config.resources.cpu_count = cpus_->value();
    config.resources.memory_percent = memory_percent_->value();
    config.resources.scratch_directory = storage::scratch_root().toStdString();
    config.step.step_time = step_time_->value();
    config.step.stabilization_magnitude = stabilization_->value();
    config.step.adaptive_damping_ratio = damping_->value();
    config.step.maximum_increments = maximum_increments_->value();
    config.step.initial_increment = initial_increment_->value();
    config.step.minimum_increment = minimum_increment_->value();
    config.step.maximum_increment = maximum_increment_->value();
    config.contact.formulation =
        static_cast<domain::ContactFormulation>(contact_->currentData().toInt());
    config.contact.friction_coefficient = friction_->value();
    config.contact.sliding =
        static_cast<domain::SlidingFormulation>(sliding_->currentData().toInt());
    config.contact.adjustment =
        static_cast<domain::ContactAdjustment>(adjustment_->currentData().toInt());
    config.contact.adjustment_tolerance = adjustment_tolerance_->value();
    config.materials = {material_a_->material(), material_b_->material()};
    config.use_target_mesh_size = target_size_enabled_->isChecked();
    config.target_mesh_size = target_size_->value();
    config.options.emplace("vertex_tolerance", std::to_string(vertex_tolerance_->value()));
    config.options.emplace("edge_tolerance", std::to_string(edge_tolerance_->value()));
    config.options.emplace("surface_tolerance", std::to_string(surface_tolerance_->value()));
    config.options.emplace("angle_tolerance", std::to_string(angle_tolerance_->value()));
    return config;
}

void SimulationPage::validate_configuration() {
    const auto config = simulation_config();
    const auto errors = config.validation_errors();
    if (!errors.empty()) {
        QMessageBox::warning(this, tr("仿真配置无效"), validation_text(errors));
        return;
    }
    log_->append(tr("✓ C++ SimulationConfig、材料、求解增量和接触参数校验通过"));
    QMessageBox::information(this, tr("配置有效"),
                             tr("配置已通过 C++ 领域规则校验，可提交给 Abaqus。"));
}

void SimulationPage::start_simulation() {
    try {
        active_lifecycle_started_ = false;
        active_task_id_.clear();
        active_result_path_.clear();
        active_manifest_path_.clear();
        active_sample_known_ = input_mode_->currentData().toString() == QStringLiteral("sample");
        active_sample_id_ = active_sample_known_ ? samples_->currentData().toString() : QString{};
        active_dataset_ = active_sample_known_
                              ? static_cast<domain::DatasetKind>(
                                    samples_->currentData(Qt::UserRole + 1).toInt())
                              : domain::DatasetKind::demo;
        active_mesh_directory_ = active_sample_known_
                                     ? known_sample_artifact_directory(context_, active_sample_id_)
                                     : external_directory_->text().trimmed();
        active_run_id_ = QStringLiteral("abaqus-%1").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
        bool legacy_sample = false;
        if (active_sample_known_) {
            const auto id = active_sample_id_.toStdString();
            const auto known = context_.datasets().list(active_dataset_);
            const auto found = std::find_if(known.begin(), known.end(), [&id](const auto& sample) {
                return sample.id == id;
            });
            legacy_sample = found != known.end() && found->source == domain::DesignSource::existing;
        }
        if (legacy_sample) {
            active_work_directory_ = QDir{storage::simulation_run_root()}.filePath(
                QStringLiteral("%1/%2").arg(active_sample_id_, active_run_id_));
        } else if (active_sample_known_) {
            active_work_directory_ = active_mesh_directory_;
        } else {
            active_work_directory_ = QDir{artifact_root()}.filePath(
                QStringLiteral("external-%1")
                    .arg(QDateTime::currentDateTimeUtc().toString(
                        QStringLiteral("yyyyMMdd-hhmmsszzz"))));
        }
        QDir{}.mkpath(active_work_directory_);
        if (legacy_sample) {
            const QDir source_mesh{active_mesh_directory_};
            const QDir work{active_work_directory_};
            for (const auto& name : source_mesh.entryList(
                     {QStringLiteral("*.inp"), QStringLiteral("*.obj"), QStringLiteral("*.json")},
                     QDir::Files)) {
                QFile::copy(source_mesh.filePath(name), work.filePath(name));
            }
        }
        if (!active_sample_known_) {
            const QDir source_mesh{active_mesh_directory_};
            const QDir archived_mesh{active_work_directory_};
            for (const auto& name : source_mesh.entryList(
                     {QStringLiteral("*.inp"), QStringLiteral("*.obj"),
                      QStringLiteral("*.json")},
                     QDir::Files)) {
                if (!QFile::copy(source_mesh.filePath(name), archived_mesh.filePath(name))) {
                    throw std::runtime_error{"cannot stage external mesh in G:/MBS: " +
                                             name.toStdString()};
                }
            }
            active_mesh_directory_ = active_work_directory_;
        }

        auto config = simulation_config();
        const auto errors = config.validation_errors();
        if (!errors.empty()) {
            throw std::invalid_argument{validation_text(errors).toStdString()};
        }
        const QDir mesh{active_mesh_directory_};
        for (const auto required : config.required_mesh_files()) {
            if (!mesh.exists(QString::fromUtf8(required.data()))) {
                throw std::runtime_error{"missing required mesh file: " +
                                         std::string{required}};
            }
        }
        // The Abaqus preprocessing script always imports the surface meshes for contact sets.
        for (const auto& name : {QStringLiteral("tpms-tri-A.inp"),
                                 QStringLiteral("tpms-tri-B.inp")}) {
            if (!mesh.exists(name)) {
                throw std::runtime_error{"missing required surface mesh: " + name.toStdString()};
            }
        }
        const auto script = abaqus_runtime_script(QStringLiteral("preprocess.py"));
        if (!QFileInfo::exists(script)) {
            throw std::runtime_error{"isolated Abaqus preprocess runtime is missing"};
        }
        const auto config_path = QDir{active_work_directory_}.filePath(
            QStringLiteral("simulation-config.json"));
        const auto request = simulation_json(config);
        write_json_file(config_path, request);
        active_odb_path_ = QDir{active_work_directory_}.filePath(
            QString::fromStdString(config.job_name) + QStringLiteral(".odb"));
        active_task_id_ = active_run_id_ + QStringLiteral("-workflow");
        const auto sample_reference = active_sample_known_
                                          ? std::optional<std::string>{
                                                active_sample_id_.toStdString()}
                                          : std::nullopt;
        context_.task_lifecycle().start(
            {.id = active_task_id_.toStdString(),
             .kind = "abaqus_workflow",
             .status = domain::TaskStatus::running,
             .sample_id = sample_reference,
             .run_id = active_run_id_.toStdString(),
             .progress = 0.0,
             .error = {}},
            {.id = active_run_id_.toStdString(),
             .kind = "abaqus_workflow",
             .status = domain::TaskStatus::running,
             .sample_id = sample_reference,
             .request_json = QString::fromUtf8(
                                 QJsonDocument{request}.toJson(QJsonDocument::Compact))
                                 .toStdString(),
             .error = {}});
        active_lifecycle_started_ = true;
        progress_->setValue(0);
        QSettings{}.setValue(QStringLiteral("abaqus_command"), abaqus_command_->text().trimmed());
        controller_.start_worker(
            {QStringLiteral("abaqus"), QStringLiteral("--operation"), QStringLiteral("simulation"),
             QStringLiteral("--task-id"), active_run_id_, QStringLiteral("--run-id"), active_run_id_,
             QStringLiteral("--sample-id"), active_sample_id_, QStringLiteral("--script"), script,
             QStringLiteral("--config"), config_path, QStringLiteral("--work-dir"),
             active_work_directory_, QStringLiteral("--command"), abaqus_command_->text().trimmed(),
             QStringLiteral("--job-name"), QString::fromStdString(config.job_name),
             QStringLiteral("--expected"), active_odb_path_, QStringLiteral("--step-time"),
             QString::number(config.step.step_time, 'g', 17)},
            tr("Abaqus 求解"));
        log_->append(tr("✓ 已原子写入仿真配置并提交 C++ Abaqus Gateway：%1")
                         .arg(active_work_directory_));
    } catch (const std::exception& error) {
        if (active_lifecycle_started_) {
            try {
                finish_simulation_lifecycle(domain::TaskStatus::failed,
                                            QString::fromUtf8(error.what()));
            } catch (...) {
                // Preserve the original launch error shown below.
            }
        }
        QMessageBox::warning(this, tr("无法开始仿真"), QString::fromUtf8(error.what()));
        active_work_directory_.clear();
    }
}

void SimulationPage::start_postprocess() {
    const auto script = abaqus_runtime_script(QStringLiteral("postprocess.py"));
    if (!QFileInfo::exists(script)) {
        throw std::runtime_error{"isolated Abaqus postprocess runtime is missing"};
    }
    active_result_path_ = QDir{active_work_directory_}.filePath(QStringLiteral("proof-result.json"));
    const auto config_path = QDir{active_work_directory_}.filePath(
        QStringLiteral("postprocess-config.json"));
    write_json_file(config_path,
                    {{QStringLiteral("schema_version"), 1},
                     {QStringLiteral("sample_id"), active_sample_id_},
                     {QStringLiteral("odb_path"), active_odb_path_},
                     {QStringLiteral("output_json"), active_result_path_},
                     {QStringLiteral("work_dir"), active_work_directory_},
                     {QStringLiteral("wth"), width_->value()},
                     {QStringLiteral("rep_z"), repeat_z_->value()}});
    controller_.start_worker(
        {QStringLiteral("abaqus"), QStringLiteral("--operation"), QStringLiteral("postprocess"),
         QStringLiteral("--task-id"), active_run_id_ + QStringLiteral("-post"),
         QStringLiteral("--run-id"), active_run_id_, QStringLiteral("--sample-id"),
         active_sample_id_, QStringLiteral("--script"), script, QStringLiteral("--config"),
         config_path, QStringLiteral("--work-dir"), active_work_directory_,
         QStringLiteral("--command"), abaqus_command_->text().trimmed(),
         QStringLiteral("--job-name"), QStringLiteral("job-intlck-tpms"),
         QStringLiteral("--expected"), active_result_path_},
        tr("Abaqus ODB 后处理"));
}

void SimulationPage::start_animation_export() {
    const auto script = abaqus_runtime_script(QStringLiteral("export_animation.py"));
    if (!QFileInfo::exists(script)) {
        throw std::runtime_error{"isolated Abaqus animation runtime is missing"};
    }
    const auto output_directory = QDir{active_work_directory_}.filePath(QStringLiteral("animation"));
    if (!QDir{}.mkpath(output_directory)) {
        throw std::runtime_error{"cannot create sample animation directory"};
    }
    active_manifest_path_ = QDir{output_directory}.filePath(QStringLiteral("animation.json"));
    const auto config_path = QDir{output_directory}.filePath(
        QStringLiteral("animation-config.json"));
    write_json_file(config_path,
                    {{QStringLiteral("schema_version"), 1},
                     {QStringLiteral("odb_path"), active_odb_path_},
                     {QStringLiteral("output_dir"), output_directory},
                     {QStringLiteral("manifest_path"), active_manifest_path_},
                     {QStringLiteral("fps"), 12}, {QStringLiteral("max_frames"), 240},
                     {QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}});
    controller_.start_worker(
        {QStringLiteral("abaqus"), QStringLiteral("--operation"), QStringLiteral("animation"),
         QStringLiteral("--task-id"), active_run_id_ + QStringLiteral("-animation"),
         QStringLiteral("--run-id"), active_run_id_, QStringLiteral("--sample-id"),
         active_sample_id_, QStringLiteral("--script"), script, QStringLiteral("--config"),
         config_path, QStringLiteral("--work-dir"), active_work_directory_,
         QStringLiteral("--command"), abaqus_command_->text().trimmed(),
         QStringLiteral("--job-name"), QStringLiteral("job-intlck-tpms"),
         QStringLiteral("--expected"), active_manifest_path_},
        tr("Abaqus 云图动画导出"));
}

void SimulationPage::finish_simulation_lifecycle(const domain::TaskStatus status,
                                                 const QString& error) {
    if (!active_lifecycle_started_) {
        return;
    }
    context_.task_lifecycle().finish(active_task_id_.toStdString(),
                                     active_run_id_.toStdString(), status,
                                     error.toStdString());
    active_lifecycle_started_ = false;
}

void SimulationPage::finish_simulation_workflow() {
    progress_->setValue(100);
    if (QFileInfo::exists(active_manifest_path_)) {
        if (player_->load_manifest(active_manifest_path_)) {
            log_->append(tr("✓ Mises 云图动画已载入。"));
        }
    }
    QFile result_file{active_result_path_};
    if (!result_file.open(QIODevice::ReadOnly)) {
        log_->append(tr("后处理结果文件不可读：%1").arg(active_result_path_));
        finish_simulation_lifecycle(domain::TaskStatus::failed,
                                    tr("后处理结果文件不可读"));
        return;
    }
    const auto result = QJsonDocument::fromJson(result_file.readAll()).object();
    const auto proof = result.value(QStringLiteral("proof_stress")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(proof) || proof < 0.0) {
        log_->append(tr("后处理结果没有有效的非负证明应力。"));
        finish_simulation_lifecycle(domain::TaskStatus::failed,
                                    tr("后处理结果没有有效的非负证明应力"));
        return;
    }
    const auto sample_reference = active_sample_known_
                                      ? std::optional<std::string>{active_sample_id_.toStdString()}
                                      : std::nullopt;
    for (const auto& [kind, path] :
         std::array{std::pair{std::string{"abaqus_odb"}, active_odb_path_},
                    std::pair{std::string{"postprocess_result"}, active_result_path_},
                    std::pair{std::string{"animation_manifest"}, active_manifest_path_}}) {
        const QFileInfo info{path};
        if (!info.isFile()) {
            continue;
        }
        static_cast<void>(context_.results().register_artifact(
            {.id = {},
             .kind = kind,
             .uri = path.toStdString(),
             .sample_id = sample_reference,
             .run_id = active_run_id_.toStdString(),
             .size_bytes = static_cast<std::uint64_t>(info.size()),
             .checksum = std::nullopt}));
    }
    static_cast<void>(context_.results().record_metric(
        {.id = {},
         .name = "proof_stress",
         .value = proof,
         .unit = "MPa",
         .sample_id = sample_reference,
         .run_id = active_run_id_.toStdString(),
         .details_json = QString::fromUtf8(QJsonDocument{result}.toJson(QJsonDocument::Compact))
                             .toStdString()}));
    if (active_sample_known_) {
        auto samples = context_.datasets().list(active_dataset_);
        const auto found = std::find_if(samples.begin(), samples.end(), [this](const auto& sample) {
            return sample.id == active_sample_id_.toStdString();
        });
        if (found != samples.end()) {
            found->status = "simulation_succeeded";
            context_.datasets().save(*found);
            if (active_dataset_ == domain::DatasetKind::mbs) {
                context_.optimization().save_observation(
                    {.id = "obs-" + found->id,
                     .sample_id = found->id,
                     .objective = -proof,
                     .parameters = found->parameters});
            }
        }
    }
    finish_simulation_lifecycle(domain::TaskStatus::succeeded);
    log_->append(tr("✓ 仿真闭环完成：ODB、证明应力 %1 MPa、动画清单均已生成。")
                     .arg(proof, 0, 'g', 8));
    const auto removed = cleanup_successful_abaqus_transients(
        active_work_directory_, QStringLiteral("job-intlck-tpms"));
    if (removed > 0) {
        log_->append(tr("✓ 已清理 %1 个成功作业的 Abaqus 中间文件；保留 INP/OBJ/ODB/JSON/PNG。")
                         .arg(removed));
    }
    refresh();
}

OptimizationPage::OptimizationPage(ApplicationContext& context, TaskController& controller,
                                   QWidget* parent)
    : QWidget(parent), context_(context), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);
    auto* metrics = new QHBoxLayout;
    metrics->addWidget(metric_card(tr("MBS + Demo 样本"), sample_count_, this));
    metrics->addWidget(metric_card(tr("有效优化观测"), observation_count_, this));
    metrics->addWidget(metric_card(tr("当前最佳证明应力"), best_result_, this));
    root->addLayout(metrics);
    auto* main_splitter = new QSplitter(Qt::Horizontal, this);
    auto* dataset_tabs = new QTabWidget(main_splitter);

    const auto build_editor = [this](const domain::DatasetKind dataset, QTableWidget*& table) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        auto* actions = new QHBoxLayout;
        auto* add = new QPushButton(tr("新增行"), page);
        auto* remove = new QPushButton(tr("删除选中行"), page);
        auto* save = new QPushButton(tr("保存"), page);
        auto* undo = new QPushButton(tr("撤销未保存修改"), page);
        auto* reload = new QPushButton(tr("刷新数据库"), page);
        auto* delete_files = new QPushButton(tr("删除选中行及磁盘文件"), page);
        delete_files->setObjectName(QStringLiteral("destructiveButton"));
        for (auto* button : {add, remove, save, undo, reload, delete_files}) {
            actions->addWidget(button);
        }
        actions->addStretch(1);
        layout->addLayout(actions);
        table = new QTableWidget(page);
        table->setColumnCount(16);
        table->setHorizontalHeaderLabels({tr("sample_id"), QStringLiteral("λ"), QStringLiteral("μ"),
                                          QStringLiteral("κ"), QStringLiteral("β"),
                                          tr("rnd_x"), tr("rnd_y"), tr("RVE / mm"),
                                          tr("Z 重复"), tr("盖板 / mm"), tr("采样分辨率"),
                                          tr("目标边长 / %RVE"), tr("证明应力 / MPa"),
                                          tr("状态"), tr("来源"), tr("制品目录")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        connect(add, &QPushButton::clicked, this,
                [this, table, dataset] { add_row(table, dataset); });
        connect(remove, &QPushButton::clicked, this,
                [this, table, dataset] { delete_rows(table, dataset); });
        connect(save, &QPushButton::clicked, this,
                [this, table, dataset] { save_table(table, dataset); });
        connect(undo, &QPushButton::clicked, this,
                [this, table, dataset] { populate_table(table, dataset); });
        connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
        connect(delete_files, &QPushButton::clicked, this,
                [this, table, dataset] { delete_rows_and_files(table, dataset); });
        return page;
    };
    dataset_tabs->addTab(build_editor(domain::DatasetKind::mbs, mbs_table_), tr("MBS 研究样本"));
    dataset_tabs->addTab(build_editor(domain::DatasetKind::demo, demo_table_), tr("Demo 手动样本"));
    main_splitter->addWidget(dataset_tabs);

    auto* analysis = new QWidget(main_splitter);
    auto* analysis_layout = new QVBoxLayout(analysis);
    auto* action_tabs = new QTabWidget(analysis);
    auto* post = new QWidget(action_tabs);
    auto* post_form = new QFormLayout(post);
    odb_path_ = new QLineEdit(post);
    auto* browse_odb = new QPushButton(tr("浏览 ODB"), post);
    auto* odb_row = new QHBoxLayout;
    odb_row->addWidget(odb_path_, 1);
    odb_row->addWidget(browse_odb);
    odb_target_ = new NoWheelComboBox(post);
    post_width_ = double_input(10.0, 0.001, 1000.0, 4, 0.5);
    post_repeat_ = integer_input(3, 1, 100);
    auto* postprocess = new QPushButton(tr("执行 ODB 后处理"), post);
    postprocess->setObjectName(QStringLiteral("primaryButton"));
    post_form->addRow(tr("ODB"), odb_row);
    post_form->addRow(tr("写回目标（可不选）"), odb_target_);
    post_form->addRow(tr("ucs = wth"), post_width_);
    post_form->addRow(tr("Z 重复数"), post_repeat_);
    post_form->addRow(postprocess);
    action_tabs->addTab(post, tr("ODB 后处理"));

    auto* bo = new QWidget(action_tabs);
    auto* bo_form = new QFormLayout(bo);
    acquisition_ = new NoWheelComboBox(bo);
    acquisition_->addItems({QStringLiteral("EI"), QStringLiteral("LCB"), QStringLiteral("PI")});
    initial_points_ = integer_input(10, 0, 1000);
    random_seed_ = integer_input(0, 0, 1'000'000);
    candidate_pool_ = integer_input(16, 1, 10'000);
    bo_kappa_ = double_input(1.96, 0.0, 100.0, 4, 0.1);
    bo_xi_ = double_input(0.01, 0.0, 100.0, 6, 0.01);
    pending_ = new QLabel(tr("无"), bo);
    pending_->setWordWrap(true);
    auto* bo_button = new QPushButton(tr("生成下一组 BO 候选"), bo);
    bo_button->setObjectName(QStringLiteral("primaryButton"));
    bo_form->addRow(tr("采集函数"), acquisition_);
    bo_form->addRow(tr("初始点阈值"), initial_points_);
    bo_form->addRow(tr("随机种子"), random_seed_);
    bo_form->addRow(tr("候选池"), candidate_pool_);
    bo_form->addRow(tr("LCB kappa"), bo_kappa_);
    bo_form->addRow(tr("EI/PI xi"), bo_xi_);
    bo_form->addRow(tr("当前候选"), pending_);
    bo_form->addRow(bo_button);
    auto* bo_ready = new QLabel(tr("C++ ARD Matérn-5/2 GP + EI/LCB/PI"), bo);
    bo_ready->setObjectName(QStringLiteral("statusPill"));
    bo_ready->setWordWrap(true);
    bo_form->addRow(bo_ready);
    action_tabs->addTab(bo, tr("贝叶斯采样"));
    analysis_layout->addWidget(action_tabs);
    auto* chart_header = new QHBoxLayout;
    chart_header->addWidget(new QLabel(tr("数据库分析"), analysis));
    chart_header->addStretch(1);
    auto* refresh_charts = new QPushButton(tr("刷新图表"), analysis);
    chart_header->addWidget(refresh_charts);
    analysis_layout->addLayout(chart_header);
    charts_ = new QTabWidget(analysis);
    convergence_ = new ConvergenceChart(charts_);
    charts_->addTab(convergence_, tr("收敛"));
    parallel_ = new ParallelCoordinatesChart(charts_);
    charts_->addTab(parallel_, tr("平行坐标"));
    surrogate_ = new SurrogateSliceChart(charts_);
    charts_->addTab(surrogate_, tr("GP代理切片"));
    stress_strain_ = new StressStrainChart(charts_);
    charts_->addTab(stress_strain_, tr("应力-应变"));
    analysis_layout->addWidget(charts_, 1);
    main_splitter->addWidget(analysis);
    main_splitter->setSizes({820, 720});
    root->addWidget(main_splitter, 1);
    log_ = new LogPanel(tr("优化与 ODB 后处理日志"), this);
    log_->setMaximumHeight(230);
    root->addWidget(log_);

    connect(browse_odb, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择 Abaqus ODB"), {}, tr("Abaqus ODB (*.odb)"));
        if (!path.isEmpty()) {
            odb_path_->setText(path);
        }
    });
    connect(postprocess, &QPushButton::clicked, this, [this] { start_odb_postprocess(); });
    connect(bo_button, &QPushButton::clicked, this, [this] {
        const auto task_id = QStringLiteral("bo-%1").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
        controller_.start_worker(
            {QStringLiteral("optimize"), QStringLiteral("--task-id"), task_id,
             QStringLiteral("--database"), path_text(context_.database_path()),
             QStringLiteral("--acquisition"), acquisition_->currentText(),
             QStringLiteral("--initial-points"), QString::number(initial_points_->value()),
             QStringLiteral("--random-seed"), QString::number(random_seed_->value()),
             QStringLiteral("--candidate-pool"), QString::number(candidate_pool_->value()),
             QStringLiteral("--kappa"), QString::number(bo_kappa_->value(), 'g', 17),
             QStringLiteral("--xi"), QString::number(bo_xi_->value(), 'g', 17)},
            tr("C++ 贝叶斯候选生成"));
    });
    connect(&controller_, &TaskController::busy_changed, this,
            [bo_button](const bool busy) { bo_button->setEnabled(!busy); });
    connect(&controller_, &TaskController::finished, this,
            [this](const bool success, const QString& label) {
                if (label != tr("C++ 贝叶斯候选生成")) {
                    return;
                }
                if (success) {
                    log_->append(tr("✓ C++ GP 已生成唯一约束候选并写入 SQLite。"));
                    refresh();
                } else {
                    log_->append(tr("候选生成失败；原待建模候选保持不变。"));
                }
            });
    connect(refresh_charts, &QPushButton::clicked, this, [this] { refresh(); });
    connect(&controller_, &TaskController::log_received, log_, &LogPanel::append);
    connect(&controller_, &TaskController::finished, this,
            [this](const bool success, const QString& label) {
                if (label == tr("ODB 独立后处理")) {
                    try {
                        finish_odb_postprocess(success);
                    } catch (const std::exception& error) {
                        log_->append(tr("ODB 结果写回失败：%1").arg(QString::fromUtf8(error.what())));
                    }
                }
            });
    refresh();
}

void OptimizationPage::populate_table(QTableWidget* table, const domain::DatasetKind dataset) {
    const auto samples = context_.datasets().list(dataset);
    const auto observations = context_.optimization().observations();
    table->setRowCount(static_cast<int>(samples.size()));
    for (int row = 0; row < static_cast<int>(samples.size()); ++row) {
        const auto& sample = samples[static_cast<std::size_t>(row)];
        const auto observation = std::find_if(
            observations.begin(), observations.end(), [&sample](const auto& value) {
                return value.sample_id == sample.id;
            });
        const std::array values{QString::fromStdString(sample.id),
                                QString::number(sample.parameters.lambda),
                                QString::number(sample.parameters.mu),
                                QString::number(sample.parameters.kappa),
                                QString::number(sample.parameters.beta),
                                QString::number(sample.parameters.phase_x, 'g', 15),
                                QString::number(sample.parameters.phase_y, 'g', 15),
                                QString::number(sample.mesh.width, 'g', 15),
                                QString::number(sample.mesh.repeat_z),
                                QString::number(sample.mesh.plate_thickness, 'g', 15),
                                QString::number(sample.mesh.resolution),
                                QString::number(sample.mesh.target_edge_percent, 'g', 15),
                                observation == observations.end()
                                    ? QString{}
                                    : QString::number(-observation->objective, 'g', 15),
                                QString::fromStdString(sample.status),
                                QString::fromUtf8(domain::to_string(sample.source).data()),
                                sample.artifact_directory
                                    ? QString::fromStdString(*sample.artifact_directory)
                                    : QString{}};
        for (int column = 0; column < static_cast<int>(values.size()); ++column) {
            table->setItem(row, column, new QTableWidgetItem(values[static_cast<std::size_t>(column)]));
        }
    }
}

void OptimizationPage::add_row(QTableWidget* table, const domain::DatasetKind dataset) {
    const auto row = table->rowCount();
    table->insertRow(row);
    const std::array values{
        QStringLiteral("user-%1").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmsszzz"))),
        QStringLiteral("0.20"), QStringLiteral("0.20"), QStringLiteral("0.50"),
        QStringLiteral("0.00"), QStringLiteral("0.00"), QStringLiteral("0.00"),
        QStringLiteral("10"), QStringLiteral("3"), QStringLiteral("1"),
        QStringLiteral("30"), QStringLiteral("5"), QString{}, QStringLiteral("data_only"),
        dataset == domain::DatasetKind::mbs ? QStringLiteral("bayesian_optimization")
                                            : QStringLiteral("manual"), QString{}};
    for (int column = 0; column < static_cast<int>(values.size()); ++column) {
        table->setItem(row, column, new QTableWidgetItem(values[static_cast<std::size_t>(column)]));
    }
}

void OptimizationPage::delete_rows(QTableWidget* table, const domain::DatasetKind) {
    auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, tr("确认删除"),
                              tr("仅从当前编辑表移除选中记录；点击“保存”后写入 SQLite。继续吗？")) !=
        QMessageBox::Yes) {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.row() > right.row();
    });
    for (const auto& index : rows) {
        table->removeRow(index.row());
    }
}

void OptimizationPage::delete_rows_and_files(QTableWidget* table,
                                              const domain::DatasetKind dataset) {
    auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    QStringList identifiers;
    for (const auto& row : rows) {
        if (const auto* item = table->item(row.row(), 0); item != nullptr) {
            identifiers.push_back(item->text().trimmed());
        }
    }
    if (QMessageBox::warning(
            this, tr("确认删除记录与制品"),
            tr("将把样本根目录中的以下永久制品目录移入系统回收站，并立即从 SQLite 删除：\n\n%1")
                .arg(identifiers.join(QStringLiteral("\n"))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const QDir root{artifact_root()};
    const QDir legacy_root{application_data_root()};
    const auto known_samples = context_.datasets().list(dataset);
    for (const auto& id : identifiers) {
        if (id.isEmpty() || id == QStringLiteral(".") || id == QStringLiteral("..") ||
            id.contains('/') || id.contains('\\')) {
            QMessageBox::warning(this, tr("拒绝删除"), tr("样本 ID 包含不安全的路径字符：%1").arg(id));
            return;
        }
        const auto native_id = id.toStdString();
        const auto known = std::find_if(known_samples.begin(), known_samples.end(),
                                        [&native_id](const auto& sample) {
                                            return sample.id == native_id;
                                        });
        QStringList paths;
        if (known != known_samples.end() && known->artifact_directory.has_value() &&
            !known->artifact_directory->empty()) {
            paths.push_back(QString::fromStdString(*known->artifact_directory));
        }
        paths.push_back(legacy_root.absoluteFilePath(QStringLiteral("samples/%1").arg(id)));
        paths.push_back(legacy_root.absoluteFilePath(QStringLiteral("runs/%1").arg(id)));
        for (const auto& path : paths) {
            if (QFileInfo::exists(path) && !QFile::moveToTrash(path)) {
                QMessageBox::warning(this, tr("删除失败"),
                                     tr("无法把目录移入回收站：%1\n数据库尚未修改。").arg(path));
                return;
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.row() > right.row();
    });
    for (const auto& row : rows) {
        table->removeRow(row.row());
    }
    save_table(table, dataset);
    log_->append(tr("样本关联目录已移入系统回收站，可在误删时恢复。"));
}

void OptimizationPage::save_table(QTableWidget* table, const domain::DatasetKind dataset) {
    std::vector<domain::Sample> samples;
    std::vector<domain::OptimizationObservation> observations_to_save;
    std::vector<std::string> observations_to_delete;
    const auto existing_samples = context_.datasets().list(dataset);
    int next_serial = 0;
    for (const auto& existing : existing_samples) {
        next_serial = std::max(next_serial, existing.serial);
    }
    samples.reserve(static_cast<std::size_t>(table->rowCount()));
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto sample_id = table->item(row, 0)->text().trimmed().toStdString();
        const auto existing = std::find_if(existing_samples.begin(), existing_samples.end(),
                                           [&sample_id](const auto& value) {
                                               return value.id == sample_id;
                                           });
        domain::Sample sample = existing == existing_samples.end() ? domain::Sample{} : *existing;
        sample.id = sample_id;
        sample.project_id = "default";
        sample.dataset = dataset;
        if (existing == existing_samples.end()) {
            sample.serial = ++next_serial;
            sample.source = dataset == domain::DatasetKind::mbs
                                ? domain::DesignSource::bayesian_optimization
                                : domain::DesignSource::manual;
        }
        sample.parameters.lambda = table->item(row, 1)->text().toDouble();
        sample.parameters.mu = table->item(row, 2)->text().toDouble();
        sample.parameters.kappa = table->item(row, 3)->text().toDouble();
        sample.parameters.beta = table->item(row, 4)->text().toDouble();
        sample.parameters.phase_x = table->item(row, 5)->text().toDouble();
        sample.parameters.phase_y = table->item(row, 6)->text().toDouble();
        sample.mesh.width = table->item(row, 7)->text().toDouble();
        sample.mesh.repeat_z = table->item(row, 8)->text().toInt();
        sample.mesh.plate_thickness = table->item(row, 9)->text().toDouble();
        sample.mesh.resolution = table->item(row, 10)->text().toInt();
        sample.mesh.target_edge_percent = table->item(row, 11)->text().toDouble();
        sample.status = table->item(row, 13)->text().trimmed().toStdString();
        const auto errors = sample.validation_errors();
        if (!errors.empty()) {
            QMessageBox::warning(this, tr("第 %1 行无效").arg(row + 1), validation_text(errors));
            return;
        }
        if (dataset == domain::DatasetKind::mbs) {
            const auto stress_text = table->item(row, 12)->text().trimmed();
            if (!stress_text.isEmpty()) {
                bool valid_stress = false;
                const auto proof_stress = stress_text.toDouble(&valid_stress);
                if (!valid_stress || !std::isfinite(proof_stress) || proof_stress < 0.0) {
                    QMessageBox::warning(this, tr("第 %1 行无效").arg(row + 1),
                                         tr("证明应力必须为空或非负有限数值。"));
                    return;
                }
                observations_to_save.push_back(
                    {.id = "obs-" + sample.id,
                     .sample_id = sample.id,
                     .objective = -proof_stress,
                     .parameters = sample.parameters});
            } else {
                observations_to_delete.push_back(sample.id);
            }
        }
        samples.push_back(std::move(sample));
    }
    context_.datasets().replace(dataset, samples);
    for (const auto& sample_id : observations_to_delete) {
        context_.optimization().delete_observation(sample_id);
    }
    for (const auto& observation : observations_to_save) {
        context_.optimization().save_observation(observation);
    }
    log_->append(tr("SQLite %1 数据表已保存，共 %2 行。")
                     .arg(QString::fromUtf8(domain::to_string(dataset).data()))
                     .arg(samples.size()));
    refresh();
}

void OptimizationPage::refresh_targets() {
    const auto current = odb_target_->currentData();
    odb_target_->clear();
    odb_target_->addItem(tr("不写回，仅查看曲线"), QString{});
    for (const auto dataset : {domain::DatasetKind::mbs, domain::DatasetKind::demo}) {
        for (const auto& sample : context_.datasets().list(dataset)) {
            odb_target_->addItem(
                QStringLiteral("%1 | %2")
                    .arg(QString::fromUtf8(domain::to_string(dataset).data()),
                         QString::fromStdString(sample.id)),
                QString::fromStdString(sample.id));
            odb_target_->setItemData(odb_target_->count() - 1, static_cast<int>(dataset),
                                     Qt::UserRole + 1);
        }
    }
    const auto index = odb_target_->findData(current);
    if (index >= 0) {
        odb_target_->setCurrentIndex(index);
    }
}

void OptimizationPage::start_odb_postprocess() {
    try {
        const auto odb = odb_path_->text().trimmed();
        if (!QFileInfo{odb}.isFile()) {
            throw std::invalid_argument{"please select an existing Abaqus ODB file"};
        }
        const auto script = abaqus_runtime_script(QStringLiteral("postprocess.py"));
        if (!QFileInfo::exists(script)) {
            throw std::runtime_error{"isolated Abaqus postprocess runtime is missing"};
        }
        const auto task_id = QStringLiteral("odb-%1").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz")));
        const auto work = QDir{temporary_workspace()}.filePath(
            QStringLiteral("_postprocess/%1").arg(task_id));
        postprocess_output_path_ = QDir{work}.filePath(QStringLiteral("proof-result.json"));
        postprocess_sample_id_ = odb_target_->currentData().toString();
        const auto config_path = QDir{work}.filePath(QStringLiteral("postprocess-config.json"));
        write_json_file(config_path,
                        {{QStringLiteral("schema_version"), 1},
                         {QStringLiteral("sample_id"), postprocess_sample_id_},
                         {QStringLiteral("odb_path"), odb},
                         {QStringLiteral("output_json"), postprocess_output_path_},
                         {QStringLiteral("work_dir"), work},
                         {QStringLiteral("wth"), post_width_->value()},
                         {QStringLiteral("rep_z"), post_repeat_->value()}});
        const auto command = QSettings{}.value(QStringLiteral("abaqus_command"),
                                                QStringLiteral("abaqus")).toString();
        controller_.start_worker(
            {QStringLiteral("abaqus"), QStringLiteral("--operation"),
             QStringLiteral("postprocess"), QStringLiteral("--task-id"), task_id,
             QStringLiteral("--run-id"), task_id, QStringLiteral("--sample-id"),
             postprocess_sample_id_, QStringLiteral("--script"), script,
             QStringLiteral("--config"), config_path, QStringLiteral("--work-dir"), work,
             QStringLiteral("--command"), command, QStringLiteral("--job-name"),
             QStringLiteral("job-intlck-tpms"), QStringLiteral("--expected"),
             postprocess_output_path_},
            tr("ODB 独立后处理"));
        log_->append(tr("已通过 C++ Gateway 以只读方式提交 ODB 后处理。"));
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("无法执行 ODB 后处理"), QString::fromUtf8(error.what()));
    }
}

void OptimizationPage::finish_odb_postprocess(const bool success) {
    if (!success) {
        log_->append(tr("ODB 后处理失败；未写入优化观测。"));
        return;
    }
    QFile file{postprocess_output_path_};
    if (!file.open(QIODevice::ReadOnly)) {
        log_->append(tr("无法读取结构化后处理结果：%1").arg(postprocess_output_path_));
        return;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        log_->append(tr("后处理 JSON 无效：%1").arg(parse_error.errorString()));
        return;
    }
    const auto result = document.object();
    const auto proof = result.value(QStringLiteral("proof_stress")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(proof) || proof < 0.0) {
        log_->append(tr("后处理结果没有有效的非负证明应力。"));
        return;
    }
    static_cast<StressStrainChart*>(stress_strain_)->set_result(result);
    charts_->setCurrentWidget(stress_strain_);
    if (!postprocess_sample_id_.isEmpty()) {
        const auto dataset = static_cast<domain::DatasetKind>(
            odb_target_->currentData(Qt::UserRole + 1).toInt());
        auto samples = context_.datasets().list(dataset);
        const auto found = std::find_if(samples.begin(), samples.end(), [this](const auto& sample) {
            return sample.id == postprocess_sample_id_.toStdString();
        });
        if (found != samples.end()) {
            found->status = "postprocessed";
            context_.datasets().save(*found);
            if (dataset == domain::DatasetKind::mbs) {
                context_.optimization().save_observation(
                    {.id = "obs-" + found->id,
                     .sample_id = found->id,
                     .objective = -proof,
                     .parameters = found->parameters});
            }
        }
    }
    const auto sample_reference = postprocess_sample_id_.isEmpty()
                                      ? std::nullopt
                                      : std::optional<std::string>{
                                            postprocess_sample_id_.toStdString()};
    const QFileInfo result_info{postprocess_output_path_};
    static_cast<void>(context_.results().register_artifact(
        {.id = {},
         .kind = "postprocess_result",
         .uri = postprocess_output_path_.toStdString(),
         .sample_id = sample_reference,
         .run_id = std::nullopt,
         .size_bytes = result_info.isFile()
                           ? std::optional<std::uint64_t>{
                                 static_cast<std::uint64_t>(result_info.size())}
                           : std::nullopt,
         .checksum = std::nullopt}));
    static_cast<void>(context_.results().record_metric(
        {.id = {},
         .name = "proof_stress",
         .value = proof,
         .unit = "MPa",
         .sample_id = sample_reference,
         .run_id = std::nullopt,
         .details_json = QString::fromUtf8(document.toJson(QJsonDocument::Compact)).toStdString()}));
    log_->append(tr("✓ ODB 后处理完成：0.2%% 偏移证明应力 %1 MPa。")
                     .arg(proof, 0, 'g', 8));
    refresh();
}

void OptimizationPage::refresh() {
    populate_table(mbs_table_, domain::DatasetKind::mbs);
    populate_table(demo_table_, domain::DatasetKind::demo);
    refresh_targets();
    const auto mbs = context_.datasets().list(domain::DatasetKind::mbs);
    const auto demo = context_.datasets().list(domain::DatasetKind::demo);
    const auto observations = context_.optimization().observations();
    sample_count_->setText(QString::number(mbs.size() + demo.size()));
    observation_count_->setText(QString::number(observations.size()));
    if (observations.empty()) {
        best_result_->setText(QStringLiteral("—"));
    } else {
        const auto best = std::min_element(observations.begin(), observations.end(),
                                           [](const auto& left, const auto& right) {
                                               return left.objective < right.objective;
                                           });
        best_result_->setText(QStringLiteral("%1 MPa").arg(-best->objective, 0, 'g', 6));
    }
    std::vector<double> values;
    values.reserve(observations.size());
    for (const auto& observation : observations) {
        values.push_back(-observation.objective);
    }
    static_cast<ConvergenceChart*>(convergence_)->set_values(std::move(values));
    static_cast<ParallelCoordinatesChart*>(parallel_)->set_observations(observations);
    if (observations.size() >= static_cast<std::size_t>(initial_points_->value()) &&
        !observations.empty()) {
        try {
            static_cast<SurrogateSliceChart*>(surrogate_)->set_slices(
                context_.optimizer().slices(observations));
        } catch (const std::exception& error) {
            static_cast<SurrogateSliceChart*>(surrogate_)->set_slices(
                std::nullopt, tr("GP 切片失败：%1").arg(QString::fromUtf8(error.what())));
        }
    } else {
        static_cast<SurrogateSliceChart*>(surrogate_)->set_slices(
            std::nullopt,
            tr("有效观测 %1 / 初始阈值 %2；达到阈值后显示四维 GP 切片。")
                .arg(observations.size())
                .arg(initial_points_->value()));
    }
    const auto pending = context_.optimization().load_pending_json();
    pending_->setText(pending_summary(pending));
}

SettingsPage::SettingsPage(ApplicationContext& context, TaskController& controller, QWidget* parent)
    : QWidget(parent), context_(context), controller_(controller) {
    auto* outer = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* root = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    auto* reload_group = new QGroupBox(tr("界面代码刷新"), content);
    auto* reload_layout = new QHBoxLayout(reload_group);
    auto* reload_text = new QLabel(
        tr("重新启动当前 C++ GUI 进程并加载最新构建；未保存的界面输入不会保留。"), reload_group);
    reload_text->setWordWrap(true);
    auto* reload = new QPushButton(tr("刷新 GUI"), reload_group);
    reload->setObjectName(QStringLiteral("reloadGuiButton"));
    reload->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    reload_layout->addWidget(reload_text, 1);
    reload_layout->addWidget(reload);
    root->addWidget(reload_group);

    auto* storage = new QGroupBox(tr("样本存储磁盘"), content);
    auto* storage_layout = new QVBoxLayout(storage);
    auto* storage_header = new QHBoxLayout;
    disk_summary_ = new QLabel(tr("正在读取磁盘容量……"), storage);
    disk_summary_->setWordWrap(true);
    auto* refresh_storage = new QPushButton(tr("刷新存储状态"), storage);
    storage_header->addWidget(disk_summary_, 1);
    storage_header->addWidget(refresh_storage);
    disk_usage_ = new QProgressBar(storage);
    disk_usage_->setRange(0, 100);
    disk_usage_->setFormat(tr("磁盘使用率 %p%"));
    storage_layout->addLayout(storage_header);
    storage_layout->addWidget(disk_usage_);
    root->addWidget(storage);

    const auto database_file = path_text(context_.database_path());
    const std::array path_rows{
        std::tuple{tr("样本根目录"), storage::artifact_root(), false,
                   tr("正式样本与网格制品目录。")},
        std::tuple{tr("设计暂存"), storage::staging_root(), false,
                   tr("几何内核的原子发布暂存目录。")},
        std::tuple{tr("遗留样本重算"), storage::simulation_run_root(), false,
                   tr("隔离保存旧样本的 Abaqus 重算文件。")},
        std::tuple{tr("GUI只读缓存"), storage::gui_cache_root(), false,
                   tr("动画帧、截图和只读派生数据。")},
        std::tuple{tr("Abaqus scratch"), storage::scratch_root(), false,
                   tr("Abaqus 临时求解目录。")},
        std::tuple{tr("几何临时工作区"), storage::temporary_workspace(), false,
                   tr("每次新建结构前清空的纯英文临时目录。")},
        std::tuple{tr("SQLite 数据库"), database_file, true,
                   tr("3.0 与 4.0 共享的样本、材料、任务、指标与优化状态数据源。")},
        std::tuple{tr("MBS 兼容导出"), storage::mbs_export_file(), true,
                   tr("4.0 独立生成的研究样本兼容导出。")},
        std::tuple{tr("Demo 兼容导出"), storage::demo_export_file(), true,
                   tr("4.0 独立生成的手动样本兼容导出。")},
        std::tuple{tr("材料库兼容导出"), storage::materials_export_file(), true,
                   tr("SQLite 材料库的人类可读兼容导出。")},
    };
    auto* paths = new QGroupBox(tr("存储位置与用途"), content);
    auto* paths_form = new QFormLayout(paths);
    for (const auto& [label, path, is_file, description] : path_rows) {
        auto* row = new QWidget(paths);
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        auto* path_line = new QHBoxLayout;
        auto* value = new QLineEdit(path, row);
        value->setReadOnly(true);
        auto* open = new QPushButton(row);
        open->setObjectName(QStringLiteral("openPathButton_") + label);
        open->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        open->setFixedWidth(38);
        path_line->addWidget(value, 1);
        path_line->addWidget(open);
        auto* details = new QLabel(description, row);
        details->setWordWrap(true);
        details->setObjectName(QStringLiteral("mutedLabel"));
        auto* status = new QLabel(tr("状态：待读取"), row);
        status->setObjectName(QStringLiteral("pathStatus"));
        status->setProperty("path", path);
        status->setProperty("isFile", is_file);
        row_layout->addLayout(path_line);
        row_layout->addWidget(details);
        row_layout->addWidget(status);
        paths_form->addRow(label, row);
        const auto folder = is_file ? QFileInfo{path}.absolutePath() : path;
        connect(open, &QPushButton::clicked, this, [this, folder] {
            QDir{}.mkpath(folder);
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });
    }
    root->addWidget(paths);

    auto* database_group = new QGroupBox(tr("SQLite 数据库状态"), content);
    auto* database_form = new QFormLayout(database_group);
    database_path_ = new QLabel(database_group);
    database_path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    database_health_ = new QLabel(database_group);
    database_counts_ = new QLabel(database_group);
    database_counts_->setWordWrap(true);
    database_form->addRow(tr("路径"), database_path_);
    database_form->addRow(tr("状态"), database_health_);
    database_form->addRow(tr("记录"), database_counts_);
    root->addWidget(database_group);

    auto* runtime_settings = new QGroupBox(tr("运行设置"), content);
    auto* runtime_form = new QFormLayout(runtime_settings);
    QSettings persisted_settings;
    abaqus_command_ = new QLineEdit(persisted_settings.value(QStringLiteral("abaqus_command"),
                                                   QStringLiteral("abaqus")).toString(), runtime_settings);
    design_reserve_ = double_input(persisted_settings.value(QStringLiteral("reserve_design_gb"), 5.0).toDouble(),
                                   0.0, 10000.0, 2, 1.0);
    simulation_reserve_ = double_input(
        persisted_settings.value(QStringLiteral("reserve_simulation_gb"), 10.0).toDouble(), 0.0, 10000.0,
        2, 1.0);
    auto* save_settings = new QPushButton(tr("保存设置"), runtime_settings);
    runtime_form->addRow(tr("Abaqus 命令"), abaqus_command_);
    runtime_form->addRow(tr("设计最小剩余空间 / GB"), design_reserve_);
    runtime_form->addRow(tr("仿真最小剩余空间 / GB"), simulation_reserve_);
    runtime_form->addRow(save_settings);
    root->addWidget(runtime_settings);

    auto* runtime = new QGroupBox(tr("C++ 桌面运行时"), content);
    auto* runtime_layout = new QVBoxLayout(runtime);
    runtime_status_ = new QLabel(tr("C++20 · Qt 6.11.2 · VTK 9.5.2 · MinGW-w64 13.1 · SQLite"), runtime);
    runtime_status_->setObjectName(QStringLiteral("statusPill"));
    auto* health = new QPushButton(tr("运行 Worker 健康检查"), runtime);
    log_ = new LogPanel(tr("运行诊断日志"), runtime);
    log_->setMaximumHeight(230);
    runtime_layout->addWidget(runtime_status_);
    runtime_layout->addWidget(health);
    runtime_layout->addWidget(log_);
    root->addWidget(runtime);
    root->addStretch(1);

    connect(reload, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("刷新 GUI"), tr("确定启动新窗口并关闭当前窗口吗？")) !=
            QMessageBox::Yes) {
            return;
        }
        if (QProcess::startDetached(QCoreApplication::applicationFilePath(), {})) {
            QApplication::quit();
        }
    });
    connect(refresh_storage, &QPushButton::clicked, this, [this] { refresh(); });
    connect(save_settings, &QPushButton::clicked, this, [this] {
        QSettings saved_settings;
        saved_settings.setValue(QStringLiteral("abaqus_command"), abaqus_command_->text().trimmed());
        saved_settings.setValue(QStringLiteral("reserve_design_gb"), design_reserve_->value());
        saved_settings.setValue(QStringLiteral("reserve_simulation_gb"), simulation_reserve_->value());
        saved_settings.sync();
        QMessageBox::information(this, tr("设置"), tr("设置已保存。"));
    });
    connect(health, &QPushButton::clicked, &controller_, &TaskController::start_health_check);
    connect(&controller_, &TaskController::log_received, log_, &LogPanel::append);
    connect(&controller_, &TaskController::finished, this,
            [this](const bool success, const QString&) {
                runtime_status_->setText(success ? tr("✓ C++ Worker、VTK 与 SQLite 运行正常")
                                                 : tr("Worker 检查失败，请查看日志"));
            });
    refresh();
}

void SettingsPage::refresh() {
    try {
        const auto status = context_.database_status();
        database_path_->setText(path_text(status.path));
        database_health_->setText(tr("完整性 %1 · Schema v%2 · %3")
                                      .arg(QString::fromStdString(status.integrity))
                                      .arg(status.schema_version)
                                      .arg(QString::fromStdString(status.journal_mode)));
        QStringList counts;
        for (const auto& [table, count] : status.table_counts) {
            counts.push_back(QStringLiteral("%1=%2").arg(QString::fromStdString(table)).arg(count));
        }
        database_counts_->setText(counts.join(QStringLiteral(" · ")));
    } catch (const std::exception& error) {
        database_health_->setText(tr("数据库状态读取失败：%1").arg(QString::fromUtf8(error.what())));
    }
    const QStorageInfo artifact_storage{storage::artifact_root()};
    if (artifact_storage.isValid() && artifact_storage.bytesTotal() > 0) {
        const auto used = artifact_storage.bytesTotal() - artifact_storage.bytesAvailable();
        disk_usage_->setValue(static_cast<int>(used * 100 / artifact_storage.bytesTotal()));
        disk_summary_->setText(tr("%1：总容量 %2 GB · 已用 %3 GB · 剩余 %4 GB")
                                   .arg(artifact_storage.rootPath())
                                   .arg(static_cast<double>(artifact_storage.bytesTotal()) / 1.0e9, 0, 'f', 1)
                                   .arg(static_cast<double>(used) / 1.0e9, 0, 'f', 1)
                                   .arg(static_cast<double>(artifact_storage.bytesAvailable()) / 1.0e9, 0, 'f', 1));
    }
    for (auto* label : findChildren<QLabel*>(QStringLiteral("pathStatus"))) {
        const auto path = label->property("path").toString();
        const QFileInfo info{path};
        if (!info.exists()) {
            label->setText(tr("状态：尚未创建"));
        } else if (label->property("isFile").toBool()) {
            label->setText(tr("状态：已存在 · %1 KB")
                               .arg(static_cast<double>(info.size()) / 1024.0, 0, 'f', 1));
        } else {
            label->setText(tr("状态：已存在 · %1 个直接子项").arg(QDir{path}.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size()));
        }
    }
}

} // namespace mbs::presentation
