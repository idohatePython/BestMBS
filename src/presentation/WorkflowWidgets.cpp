#include "presentation/WorkflowWidgets.hpp"

#include "presentation/ApplicationContext.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>

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

QDoubleSpinBox* double_input(const double value, const double minimum, const double maximum,
                             const int decimals = 4, const double step = 0.01) {
    auto* input = new NoWheelDoubleSpinBox;
    input->setRange(minimum, maximum);
    input->setDecimals(decimals);
    input->setSingleStep(step);
    input->setValue(value);
    input->setKeyboardTracking(false);
    return input;
}

QString model_label(const domain::HyperelasticModel model) {
    switch (model) {
    case domain::HyperelasticModel::mooney_rivlin:
        return QStringLiteral("Mooney–Rivlin（一阶）");
    case domain::HyperelasticModel::neo_hooke:
        return QStringLiteral("Neo-Hooke（一阶）");
    case domain::HyperelasticModel::yeoh:
        return QStringLiteral("Yeoh（三阶）");
    case domain::HyperelasticModel::ogden:
        return QStringLiteral("Ogden");
    }
    return {};
}

} // namespace

LogPanel::LogPanel(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    auto* heading = new QLabel(title, this);
    heading->setObjectName(QStringLiteral("logTitle"));
    actions->addWidget(heading);
    actions->addStretch(1);
    auto* copy = new QPushButton(tr("复制"), this);
    auto* clear = new QPushButton(tr("清空"), this);
    auto* save = new QPushButton(tr("保存"), this);
    for (auto* button : {copy, clear, save}) {
        button->setFixedSize(48, 24);
        button->setObjectName(QStringLiteral("compactLogButton"));
    }
    actions->addWidget(copy);
    actions->addWidget(clear);
    actions->addWidget(save);
    layout->addLayout(actions);
    editor_ = new QPlainTextEdit(this);
    editor_->setReadOnly(true);
    editor_->setMaximumBlockCount(1500);
    editor_->setMinimumHeight(130);
    layout->addWidget(editor_, 1);
    connect(copy, &QPushButton::clicked, editor_, &QPlainTextEdit::selectAll);
    connect(copy, &QPushButton::clicked, editor_, &QPlainTextEdit::copy);
    connect(clear, &QPushButton::clicked, editor_, &QPlainTextEdit::clear);
    connect(save, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, tr("保存日志"), QStringLiteral("mbs-log.txt"), tr("文本文件 (*.txt)"));
        if (path.isEmpty()) {
            return;
        }
        QFile file{path};
        if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            file.write(editor_->toPlainText().toUtf8());
        }
    });
}

void LogPanel::append(const QString& message) { editor_->appendPlainText(message); }

QPlainTextEdit* LogPanel::editor() const noexcept { return editor_; }

struct MaterialEditor::Impl final {
    explicit Impl(ApplicationContext& value) : context(value) {}

    ApplicationContext& context;
    QComboBox* library{};
    QLineEdit* name{};
    QGroupBox* density_group{};
    QDoubleSpinBox* density{};
    QGroupBox* elastic_group{};
    QDoubleSpinBox* young{};
    QDoubleSpinBox* poisson{};
    QGroupBox* plastic_group{};
    QTableWidget* plastic_table{};
    QGroupBox* hyper_group{};
    QComboBox* hyper_model{};
    QSpinBox* hyper_order{};
    QWidget* coefficient_widget{};
    QFormLayout* coefficient_form{};
    std::map<std::string, QDoubleSpinBox*, std::less<>> coefficients;
    QString saved_name;
    bool loading{};
};

MaterialEditor::MaterialEditor(ApplicationContext& context, const QString& initial_name,
                               QWidget* parent)
    : QGroupBox(tr("材料配置"), parent), impl_(std::make_unique<Impl>(context)) {
    auto* layout = new QVBoxLayout(this);
    auto* library_row = new QHBoxLayout;
    impl_->library = new NoWheelComboBox(this);
    auto* create = new QPushButton(tr("新建"), this);
    auto* save_as = new QPushButton(tr("另存为"), this);
    auto* update = new QPushButton(tr("更新"), this);
    library_row->addWidget(new QLabel(tr("材料库"), this));
    library_row->addWidget(impl_->library, 1);
    library_row->addWidget(create);
    library_row->addWidget(save_as);
    library_row->addWidget(update);
    layout->addLayout(library_row);
    auto* name_form = new QFormLayout;
    impl_->name = new QLineEdit(this);
    name_form->addRow(tr("材料名称"), impl_->name);
    layout->addLayout(name_form);

    impl_->density_group = new QGroupBox(tr("密度行为"), this);
    impl_->density_group->setCheckable(true);
    auto* density_form = new QFormLayout(impl_->density_group);
    impl_->density = double_input(0.0, 0.0, 1.0, 12, 1.0e-10);
    density_form->addRow(tr("密度 / t/mm³"), impl_->density);
    layout->addWidget(impl_->density_group);

    impl_->elastic_group = new QGroupBox(tr("线弹性行为"), this);
    impl_->elastic_group->setCheckable(true);
    auto* elastic_form = new QFormLayout(impl_->elastic_group);
    impl_->young = double_input(1000.0, 0.001, 1.0e9, 4, 10.0);
    impl_->poisson = double_input(0.3, -0.9999, 0.4999, 4, 0.01);
    elastic_form->addRow(tr("杨氏模量 / MPa"), impl_->young);
    elastic_form->addRow(tr("泊松比"), impl_->poisson);
    layout->addWidget(impl_->elastic_group);

    impl_->plastic_group = new QGroupBox(tr("塑性行为"), this);
    impl_->plastic_group->setCheckable(true);
    auto* plastic_layout = new QVBoxLayout(impl_->plastic_group);
    auto* plastic_note = new QLabel(tr("首行塑性应变必须为 0。"), impl_->plastic_group);
    plastic_note->setObjectName(QStringLiteral("mutedLabel"));
    impl_->plastic_table = new QTableWidget(0, 2, impl_->plastic_group);
    impl_->plastic_table->setHorizontalHeaderLabels(
        {tr("屈服应力 / MPa"), tr("塑性应变")});
    impl_->plastic_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    impl_->plastic_table->setMaximumHeight(170);
    auto* plastic_actions = new QHBoxLayout;
    auto* add_point = new QPushButton(tr("添加点"), impl_->plastic_group);
    auto* remove_point = new QPushButton(tr("删除点"), impl_->plastic_group);
    plastic_actions->addWidget(add_point);
    plastic_actions->addWidget(remove_point);
    plastic_actions->addStretch(1);
    plastic_layout->addWidget(plastic_note);
    plastic_layout->addWidget(impl_->plastic_table);
    plastic_layout->addLayout(plastic_actions);
    layout->addWidget(impl_->plastic_group);

    impl_->hyper_group = new QGroupBox(tr("超弹性行为（各向同性，系数输入）"), this);
    impl_->hyper_group->setCheckable(true);
    auto* hyper_layout = new QVBoxLayout(impl_->hyper_group);
    auto* hyper_form = new QFormLayout;
    impl_->hyper_model = new NoWheelComboBox(impl_->hyper_group);
    for (const auto model : {domain::HyperelasticModel::mooney_rivlin,
                             domain::HyperelasticModel::neo_hooke,
                             domain::HyperelasticModel::yeoh,
                             domain::HyperelasticModel::ogden}) {
        impl_->hyper_model->addItem(model_label(model), static_cast<int>(model));
    }
    impl_->hyper_order = new NoWheelSpinBox;
    impl_->hyper_order->setRange(1, 6);
    impl_->hyper_order->setValue(4);
    hyper_form->addRow(tr("应变能模型"), impl_->hyper_model);
    hyper_form->addRow(tr("Ogden 阶数"), impl_->hyper_order);
    hyper_layout->addLayout(hyper_form);
    impl_->coefficient_widget = new QWidget(impl_->hyper_group);
    impl_->coefficient_form = new QFormLayout(impl_->coefficient_widget);
    hyper_layout->addWidget(impl_->coefficient_widget);
    layout->addWidget(impl_->hyper_group);

    connect(impl_->library, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { load_selected(); });
    connect(create, &QPushButton::clicked, this, [this] { new_material(); });
    connect(save_as, &QPushButton::clicked, this, [this] { save_material(true); });
    connect(update, &QPushButton::clicked, this, [this] { save_material(false); });
    connect(add_point, &QPushButton::clicked, this, [this] {
        const auto row = impl_->plastic_table->rowCount();
        impl_->plastic_table->insertRow(row);
        impl_->plastic_table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("0")));
        impl_->plastic_table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0")));
    });
    connect(remove_point, &QPushButton::clicked, this, [this] {
        const auto row = impl_->plastic_table->currentRow();
        if (row >= 0) {
            impl_->plastic_table->removeRow(row);
        }
    });
    connect(impl_->elastic_group, &QGroupBox::toggled, this,
            [this] { sync_behavior_groups(); });
    connect(impl_->plastic_group, &QGroupBox::toggled, this,
            [this] { sync_behavior_groups(); });
    connect(impl_->hyper_group, &QGroupBox::toggled, this,
            [this] { sync_behavior_groups(); });
    connect(impl_->hyper_model, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { rebuild_hyperelastic_fields(); });
    connect(impl_->hyper_order, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { rebuild_hyperelastic_fields(); });
    reload_library(initial_name);
}

MaterialEditor::~MaterialEditor() = default;

void MaterialEditor::reload_library(const QString& preferred_name) {
    const auto library = impl_->context.materials().load();
    const auto selection = preferred_name.isEmpty() ? impl_->saved_name : preferred_name;
    impl_->loading = true;
    impl_->library->clear();
    for (const auto& material : library.materials) {
        impl_->library->addItem(QString::fromStdString(material.name));
    }
    auto index = impl_->library->findText(selection, Qt::MatchFixedString);
    if (index < 0 && impl_->library->count() > 0) {
        index = 0;
    }
    impl_->library->setCurrentIndex(index);
    impl_->loading = false;
    load_selected();
}

void MaterialEditor::load_selected() {
    if (impl_->loading || impl_->library->currentIndex() < 0) {
        return;
    }
    const auto library = impl_->context.materials().load();
    const auto index = impl_->library->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= library.materials.size()) {
        return;
    }
    const auto& value = library.materials[static_cast<std::size_t>(index)];
    impl_->loading = true;
    impl_->saved_name = QString::fromStdString(value.name);
    impl_->name->setText(impl_->saved_name);
    impl_->density_group->setChecked(value.density.enabled);
    impl_->density->setValue(value.density.value);
    impl_->elastic_group->setChecked(value.elastic.enabled);
    impl_->young->setValue(value.elastic.youngs_modulus);
    impl_->poisson->setValue(value.elastic.poissons_ratio);
    impl_->plastic_group->setChecked(value.plastic.enabled);
    impl_->plastic_table->setRowCount(0);
    for (const auto& point : value.plastic.table) {
        const auto row = impl_->plastic_table->rowCount();
        impl_->plastic_table->insertRow(row);
        impl_->plastic_table->setItem(row, 0,
                                      new QTableWidgetItem(QString::number(point.yield_stress)));
        impl_->plastic_table->setItem(row, 1,
                                      new QTableWidgetItem(QString::number(point.plastic_strain)));
    }
    impl_->hyper_group->setChecked(value.hyperelastic.enabled);
    impl_->hyper_model->setCurrentIndex(
        impl_->hyper_model->findData(static_cast<int>(value.hyperelastic.model)));
    impl_->hyper_order->setValue(value.hyperelastic.order);
    rebuild_hyperelastic_fields();
    for (const auto& [name, coefficient] : value.hyperelastic.coefficients) {
        if (const auto found = impl_->coefficients.find(name); found != impl_->coefficients.end()) {
            found->second->setValue(coefficient);
        }
    }
    impl_->loading = false;
    sync_behavior_groups();
}

void MaterialEditor::new_material() {
    impl_->saved_name.clear();
    impl_->library->setCurrentIndex(-1);
    impl_->name->setText(tr("新材料"));
    impl_->density_group->setChecked(false);
    impl_->elastic_group->setChecked(true);
    impl_->plastic_group->setChecked(false);
    impl_->hyper_group->setChecked(false);
    impl_->young->setValue(1000.0);
    impl_->poisson->setValue(0.3);
    impl_->plastic_table->setRowCount(1);
    impl_->plastic_table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("1")));
    impl_->plastic_table->setItem(0, 1, new QTableWidgetItem(QStringLiteral("0")));
    impl_->name->selectAll();
    impl_->name->setFocus();
}

domain::MaterialDefinition MaterialEditor::material() const {
    domain::MaterialDefinition value;
    value.name = impl_->name->text().trimmed().toStdString();
    value.density = {.enabled = impl_->density_group->isChecked(),
                     .value = impl_->density->value()};
    value.elastic = {.enabled = impl_->elastic_group->isChecked(),
                     .youngs_modulus = impl_->young->value(),
                     .poissons_ratio = impl_->poisson->value()};
    value.plastic.enabled = impl_->plastic_group->isChecked();
    for (int row = 0; row < impl_->plastic_table->rowCount(); ++row) {
        const auto* stress = impl_->plastic_table->item(row, 0);
        const auto* strain = impl_->plastic_table->item(row, 1);
        if (stress != nullptr && strain != nullptr) {
            value.plastic.table.push_back(
                {.yield_stress = stress->text().toDouble(),
                 .plastic_strain = strain->text().toDouble()});
        }
    }
    value.hyperelastic.enabled = impl_->hyper_group->isChecked();
    value.hyperelastic.model =
        static_cast<domain::HyperelasticModel>(impl_->hyper_model->currentData().toInt());
    value.hyperelastic.order = impl_->hyper_order->value();
    for (const auto& [name, input] : impl_->coefficients) {
        value.hyperelastic.coefficients.emplace(name, input->value());
    }
    return value;
}

void MaterialEditor::save_material(const bool save_as) {
    auto value = material();
    const auto errors = value.validation_errors();
    if (!errors.empty()) {
        QMessageBox::warning(this, tr("材料无效"), QString::fromStdString(errors.front()));
        return;
    }
    auto library = impl_->context.materials().load();
    const auto same_name = [&value](const domain::MaterialDefinition& candidate) {
        return QString::fromStdString(candidate.name).compare(
                   QString::fromStdString(value.name), Qt::CaseInsensitive) == 0;
    };
    const auto existing = std::find_if(library.materials.begin(), library.materials.end(), same_name);
    if (save_as && existing != library.materials.end()) {
        QMessageBox::warning(this, tr("材料已存在"), tr("请更换名称或使用“更新”。"));
        return;
    }
    if (!save_as) {
        const auto old_name = impl_->saved_name;
        const auto target = std::find_if(
            library.materials.begin(), library.materials.end(), [&old_name](const auto& candidate) {
                return QString::fromStdString(candidate.name).compare(old_name, Qt::CaseInsensitive) ==
                       0;
            });
        if (target == library.materials.end()) {
            QMessageBox::warning(this, tr("无法更新"), tr("请先使用“另存为”。"));
            return;
        }
        *target = value;
    } else {
        library.materials.push_back(value);
    }
    impl_->context.materials().replace(library);
    reload_library(QString::fromStdString(value.name));
    QMessageBox::information(this, tr("材料库"), tr("材料已保存到 SQLite。"));
}

void MaterialEditor::rebuild_hyperelastic_fields() {
    std::map<std::string, double, std::less<>> old;
    for (const auto& [name, input] : impl_->coefficients) {
        old.emplace(name, input->value());
    }
    while (impl_->coefficient_form->rowCount() > 0) {
        impl_->coefficient_form->removeRow(0);
    }
    impl_->coefficients.clear();
    const auto model =
        static_cast<domain::HyperelasticModel>(impl_->hyper_model->currentData().toInt());
    const auto order = model == domain::HyperelasticModel::ogden ? impl_->hyper_order->value() : 1;
    for (const auto& name : domain::coefficient_names(model, order)) {
        auto* input = double_input(old.contains(name) ? old.at(name) : 0.0, -1.0e12, 1.0e12, 9,
                                   0.01);
        if (name.starts_with('D')) {
            input->setMinimum(0.0);
        }
        impl_->coefficients.emplace(name, input);
        impl_->coefficient_form->addRow(QString::fromStdString(name), input);
    }
    impl_->hyper_order->setVisible(model == domain::HyperelasticModel::ogden);
}

void MaterialEditor::sync_behavior_groups() {
    if (impl_->loading) {
        return;
    }
    impl_->loading = true;
    if (impl_->hyper_group->isChecked()) {
        impl_->elastic_group->setChecked(false);
        impl_->plastic_group->setChecked(false);
    } else if (impl_->plastic_group->isChecked()) {
        impl_->elastic_group->setChecked(true);
    }
    impl_->elastic_group->setEnabled(!impl_->hyper_group->isChecked());
    impl_->plastic_group->setEnabled(!impl_->hyper_group->isChecked());
    impl_->loading = false;
}

FramePlayer::FramePlayer(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    image_ = new QLabel(tr("仿真成功后在此播放 Mises 云图"), this);
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumHeight(360);
    image_->setStyleSheet(QStringLiteral("background:#16202b;color:#b8c8d8;border-radius:7px"));
    layout->addWidget(image_, 1);
    auto* controls = new QHBoxLayout;
    play_ = new QPushButton(tr("播放"), this);
    play_->setCheckable(true);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, 0);
    time_ = new QLabel(QStringLiteral("0 / 0"), this);
    fps_ = new NoWheelSpinBox;
    fps_->setRange(1, 60);
    fps_->setValue(12);
    fps_->setSuffix(QStringLiteral(" FPS"));
    auto* load = new QPushButton(tr("加载已有动画"), this);
    open_directory_ = new QPushButton(tr("打开帧目录"), this);
    open_directory_->setEnabled(false);
    for (auto* widget : std::array<QWidget*, 6>{play_, slider_, time_, fps_, load,
                                                open_directory_}) {
        controls->addWidget(widget);
    }
    layout->addLayout(controls);
    timer_ = new QTimer(this);
    connect(play_, &QPushButton::toggled, this,
            [this](const bool playing) { toggle_playback(playing); });
    connect(slider_, &QSlider::valueChanged, this, [this](const int index) { show_frame(index); });
    connect(load, &QPushButton::clicked, this, [this] { browse_manifest(); });
    connect(open_directory_, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory_));
    });
    connect(timer_, &QTimer::timeout, this, [this] {
        if (!frames_.isEmpty()) {
            slider_->setValue((slider_->value() + 1) % static_cast<int>(frames_.size()));
        }
    });
}

bool FramePlayer::load_manifest(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return false;
    }
    const auto root = document.object();
    directory_ = QFileInfo{path}.absolutePath();
    frames_.clear();
    for (const auto item : root.value(QStringLiteral("frames")).toArray()) {
        const auto frame = item.toObject();
        const auto filename = frame.value(QStringLiteral("file")).toString();
        if (!filename.isEmpty()) {
            frames_.push_back({QFileInfo{directory_, filename}.absoluteFilePath(),
                               frame.value(QStringLiteral("time")).toDouble()});
        }
    }
    if (frames_.isEmpty()) {
        return false;
    }
    fps_->setValue(root.value(QStringLiteral("fps")).toInt(12));
    slider_->setRange(0, static_cast<int>(frames_.size()) - 1);
    open_directory_->setEnabled(true);
    image_->setStyleSheet(QStringLiteral("background:white;color:#263238;border-radius:7px"));
    slider_->setValue(0);
    show_frame(0);
    return true;
}

void FramePlayer::browse_manifest() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("选择已有动画清单"), {}, tr("动画清单 (animation.json);;JSON (*.json)"));
    if (!path.isEmpty() && !load_manifest(path)) {
        QMessageBox::warning(this, tr("动画加载失败"), tr("清单无效或没有帧记录。"));
    }
}

void FramePlayer::show_frame(const int index) {
    if (index < 0 || index >= frames_.size()) {
        return;
    }
    const QPixmap pixmap{frames_[index].path};
    if (!pixmap.isNull()) {
        image_->setPixmap(pixmap.scaled(image_->size(), Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
    }
    time_->setText(QStringLiteral("%1 / %2   t=%3")
                       .arg(index + 1)
                       .arg(frames_.size())
                       .arg(frames_[index].time, 0, 'g', 5));
}

void FramePlayer::toggle_playback(const bool playing) {
    play_->setText(playing ? tr("暂停") : tr("播放"));
    if (playing && !frames_.isEmpty()) {
        timer_->start(std::max(10, 1000 / fps_->value()));
    } else {
        timer_->stop();
    }
}

} // namespace mbs::presentation
