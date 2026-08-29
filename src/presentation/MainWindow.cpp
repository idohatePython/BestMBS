#include "presentation/MainWindow.hpp"

#include "presentation/Pages.hpp"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace mbs::presentation {
namespace {

QString application_style() {
    return QStringLiteral(R"(
QMainWindow, QWidget { background:#f3f6fa; color:#263238; font-size:13px; }
QLabel { background:transparent; }
QWidget#header { background:#10253f; border-radius:8px; }
QLabel#appTitle { color:white; font-size:19px; font-weight:700; }
QLabel#appSubtitle { color:#b8c8dc; font-size:11px; }
QLabel#runtimeBadge { color:#d9f1ff; background:#173f63; border:1px solid #2e6f9e;
                      border-radius:10px; padding:3px 9px; font-size:11px; font-weight:600; }
QTabWidget::pane { border:1px solid #d5dde7; background:white; border-radius:7px; top:-1px; }
QTabBar::tab { min-width:104px; padding:7px 14px; margin-right:2px; color:#455a64; font-size:12px;
               background:#e8eef5; border-top-left-radius:6px; border-top-right-radius:6px; }
QTabBar::tab:selected { background:#1565c0; color:white; font-weight:700; }
QTabBar::tab:hover:!selected { background:#d8e6f4; }
QGroupBox { font-weight:650; border:1px solid #d5dde7; border-radius:6px; margin-top:10px;
            padding:8px 6px 6px 6px; background:white; }
QGroupBox::title { subcontrol-origin:margin; left:11px; padding:0 5px; color:#29455f; }
QPushButton, QToolButton { min-height:28px; padding:2px 10px; border:1px solid #b8c5d1;
                          border-radius:4px; background:white; color:#29455f; }
QPushButton:hover, QToolButton:hover { border-color:#1976d2; background:#eaf4ff; }
QPushButton:disabled { color:#9aa5ae; background:#eef1f4; }
QPushButton#primaryButton { color:white; background:#1565c0; border-color:#0d47a1; font-weight:700; }
QPushButton#primaryButton:hover { background:#1976d2; }
QPushButton#destructiveButton { color:#b71c1c; border-color:#d32f2f; }
QPushButton#destructiveButton:hover { color:white; background:#c62828; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QListWidget, QTableWidget {
    background:white; border:1px solid #bcc8d3; border-radius:3px; padding:3px;
    selection-background-color:#1976d2; }
QPlainTextEdit { font-family:"Cascadia Mono", "Consolas"; font-size:12px; }
QHeaderView::section { background:#e9eff5; color:#37474f; padding:5px; border:0;
                       border-right:1px solid #d4dde5; font-weight:650; }
QProgressBar { border:1px solid #bdc9d4; border-radius:5px; text-align:center; background:white; }
QProgressBar::chunk { background:#1976d2; border-radius:4px; }
QLabel#pageHeading { font-size:21px; font-weight:700; color:#183a5a; }
QLabel#sectionTitle { font-size:15px; font-weight:700; color:#29455f; }
QLabel#logTitle { font-size:13px; font-weight:650; color:#29455f; }
QLabel#mutedLabel { color:#667b8d; }
QLabel#statusPill { color:#075985; background:#e0f2fe; border:1px solid #7dd3fc;
                    border-radius:6px; padding:7px; }
QLabel#stageBadge { color:#8a4b08; background:#fff7e6; border:1px solid #f0c36a;
                    border-radius:6px; padding:7px; }
QLabel#chartPlaceholder { color:#607d8b; background:#f8fafc; border:1px dashed #b0bec5;
                          border-radius:7px; font-size:14px; }
QFrame#metricCard { background:white; border:1px solid #d5dde7; border-radius:8px; min-height:80px; }
QLabel#metricValue { color:#1565c0; font-size:23px; font-weight:750; }
QStatusBar { background:#e8eef5; color:#455a64; }
QSplitter::handle { background:#dce4ec; width:2px; height:2px; }
QPushButton#compactLogButton { min-height:24px; max-height:24px; padding:0 7px; }
)");
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), context_(), controller_(this) {
    setWindowTitle(QStringLiteral("TPMS 轻量化机械互锁结构设计与优化一体化平台 · C++ 4.0"));
    resize(1640, 960);
    setMinimumSize(1180, 760);
    setStyleSheet(application_style());

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    auto* header = new QWidget(central);
    header->setObjectName(QStringLiteral("header"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(18, 7, 18, 7);
    auto* title_column = new QVBoxLayout;
    title_column->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("Mechanical Bonding Structure 4.0"), header);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("TPMS 参数设计 · 网格可视化 · Abaqus 仿真 · 贝叶斯优化闭环"), header);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    title_column->addWidget(title);
    title_column->addWidget(subtitle);
    header_layout->addLayout(title_column);
    header_layout->addStretch(1);
    auto* badge = new QLabel(QStringLiteral("C++20  ·  Qt 6  ·  VTK 9.5  ·  SQLite"), header);
    badge->setObjectName(QStringLiteral("runtimeBadge"));
    header_layout->addWidget(badge);
    root->addWidget(header);

    tabs_ = new QTabWidget(central);
    auto* design = new DesignPage(context_, controller_, tabs_);
    auto* simulation = new SimulationPage(context_, controller_, tabs_);
    auto* optimization = new OptimizationPage(context_, controller_, tabs_);
    auto* settings = new SettingsPage(context_, controller_, tabs_);
    tabs_->addTab(design, QStringLiteral("01  设计"));
    tabs_->addTab(simulation, QStringLiteral("02  仿真"));
    tabs_->addTab(optimization, QStringLiteral("03  优化"));
    tabs_->addTab(settings, QStringLiteral("设置"));
    root->addWidget(tabs_, 1);
    setCentralWidget(central);

    task_status_ = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addPermanentWidget(task_status_);
    const auto database = context_.database_status();
    statusBar()->showMessage(QStringLiteral("SQLite 完整性：%1 · Schema v%2")
                                 .arg(QString::fromStdString(database.integrity))
                                 .arg(database.schema_version));

    connect(&controller_, &TaskController::busy_changed, this, [this](const bool busy) {
        task_status_->setText(busy ? QStringLiteral("● Worker 正在运行") : QStringLiteral("就绪"));
    });
    connect(
        &controller_, &TaskController::event_received, this,
        [this](const QString&, const QString& task, const QString& message, const int progress) {
            const auto progress_text =
                progress >= 0 ? QStringLiteral(" · %1%").arg(progress) : QString{};
            statusBar()->showMessage(QStringLiteral("%1：%2%3").arg(task, message, progress_text));
        });
    connect(tabs_, &QTabWidget::currentChanged, this,
            [simulation, optimization, settings](const int index) {
                if (index == 1) {
                    simulation->refresh();
                } else if (index == 2) {
                    optimization->refresh();
                } else if (index == 3) {
                    settings->refresh();
                }
            });
}

void MainWindow::select_page(const int index) {
    if (index >= 0 && index < tabs_->count()) {
        tabs_->setCurrentIndex(index);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!controller_.busy()) {
        event->accept();
        return;
    }
    const auto answer =
        QMessageBox::question(this, QStringLiteral("任务仍在运行"),
                              QStringLiteral("关闭窗口将终止当前 Worker 任务，是否继续？"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        controller_.stop();
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace mbs::presentation
