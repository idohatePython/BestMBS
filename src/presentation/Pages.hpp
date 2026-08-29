#pragma once

#include "mbs/domain/Workflow.hpp"

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;

namespace mbs::presentation {

class ApplicationContext;
class FramePlayer;
class LogPanel;
class MaterialEditor;
class MeshViewport;
class TaskController;

class DesignPage final : public QWidget {
  public:
    DesignPage(ApplicationContext& context, TaskController& controller, QWidget* parent = nullptr);
    [[nodiscard]] MeshViewport* viewport() const noexcept;

  private:
    ApplicationContext& context_;
    TaskController& controller_;
    MeshViewport* viewport_{};
    QComboBox* source_{};
    QComboBox* history_{};
    QComboBox* acquisition_{};
    QLabel* pending_{};
    QDoubleSpinBox* lambda_{};
    QDoubleSpinBox* mu_{};
    QDoubleSpinBox* kappa_{};
    QDoubleSpinBox* beta_{};
    QDoubleSpinBox* phase_x_{};
    QDoubleSpinBox* phase_y_{};
    QDoubleSpinBox* width_{};
    QDoubleSpinBox* plate_thickness_{};
    QDoubleSpinBox* edge_percent_{};
    QDoubleSpinBox* surface_tolerance_{};
    QDoubleSpinBox* minimum_edge_percent_{};
    QDoubleSpinBox* maximum_edge_percent_{};
    QDoubleSpinBox* simplify_keep_percent_{};
    QDoubleSpinBox* feature_angle_{};
    QDoubleSpinBox* tet_dihedral_{};
    QDoubleSpinBox* tet_ratio_{};
    QDoubleSpinBox* tet_target_edge_{};
    QSpinBox* repeat_z_{};
    QSpinBox* resolution_{};
    QSpinBox* remesh_iterations_{};
    QSpinBox* repair_rounds_{};
    QSpinBox* attempts_{};
    QSpinBox* tet_optimization_{};
    QComboBox* sizing_mode_{};
    QComboBox* part_b_construction_{};
    QComboBox* tet_order_{};
    QCheckBox* random_phase_{};
    QCheckBox* tetrahedralize_{};
    QCheckBox* sharpen_{};
    QCheckBox* simplify_{};
    QCheckBox* tet_no_bisect_{};
    QCheckBox* tet_quality_{};
    QLabel* memory_{};
    QLabel* scene_status_{};
    LogPanel* log_{};
    QPlainTextEdit* object_info_{};
    QString current_directory_;
    QString prepared_sample_id_;
    QString active_geometry_sample_id_;
    QString active_geometry_archive_directory_;
    domain::DatasetKind active_geometry_dataset_{domain::DatasetKind::demo};
    domain::DesignConfig active_geometry_config_{};

    [[nodiscard]] domain::DesignConfig design_config() const;
    void update_preview();
    void update_memory();
    void save_sample();
    void refresh_history();
    void load_history();
    void stage_notice(int stage, const QString& feature);
};

class SimulationPage final : public QWidget {
  public:
    SimulationPage(ApplicationContext& context, TaskController& controller,
                   QWidget* parent = nullptr);
    void refresh();

  private:
    ApplicationContext& context_;
    TaskController& controller_;
    QComboBox* input_mode_{};
    QComboBox* samples_{};
    QLineEdit* external_directory_{};
    QComboBox* backend_{};
    QLabel* capabilities_{};
    QGroupBox* abaqus_mesh_group_{};
    QCheckBox* target_size_enabled_{};
    QDoubleSpinBox* target_size_{};
    QLineEdit* abaqus_command_{};
    QDoubleSpinBox* width_{};
    QSpinBox* repeat_z_{};
    QDoubleSpinBox* plate_thickness_{};
    QLabel* geometry_source_{};
    QDoubleSpinBox* step_time_{};
    QDoubleSpinBox* displacement_{};
    QSpinBox* cpus_{};
    QSpinBox* memory_percent_{};
    QGroupBox* advanced_{};
    QPushButton* advanced_toggle_{};
    MaterialEditor* material_a_{};
    MaterialEditor* material_b_{};
    QDoubleSpinBox* stabilization_{};
    QDoubleSpinBox* damping_{};
    QSpinBox* maximum_increments_{};
    QDoubleSpinBox* initial_increment_{};
    QDoubleSpinBox* minimum_increment_{};
    QDoubleSpinBox* maximum_increment_{};
    QComboBox* contact_{};
    QDoubleSpinBox* friction_{};
    QComboBox* sliding_{};
    QComboBox* adjustment_{};
    QDoubleSpinBox* adjustment_tolerance_{};
    QDoubleSpinBox* vertex_tolerance_{};
    QDoubleSpinBox* edge_tolerance_{};
    QDoubleSpinBox* surface_tolerance_{};
    QDoubleSpinBox* angle_tolerance_{};
    QProgressBar* progress_{};
    FramePlayer* player_{};
    LogPanel* log_{};
    QString active_sample_id_;
    QString active_task_id_;
    QString active_run_id_;
    QString active_mesh_directory_;
    QString active_work_directory_;
    QString active_odb_path_;
    QString active_result_path_;
    QString active_manifest_path_;
    domain::DatasetKind active_dataset_{domain::DatasetKind::mbs};
    bool active_sample_known_{};
    bool active_lifecycle_started_{};

    [[nodiscard]] domain::SimulationConfig simulation_config() const;
    void validate_configuration();
    void update_input_state();
    void start_simulation();
    void start_postprocess();
    void start_animation_export();
    void finish_simulation_workflow();
    void finish_simulation_lifecycle(domain::TaskStatus status, const QString& error = {});
};

class OptimizationPage final : public QWidget {
  public:
    OptimizationPage(ApplicationContext& context, TaskController& controller,
                     QWidget* parent = nullptr);
    void refresh();

  private:
    ApplicationContext& context_;
    TaskController& controller_;
    QLabel* sample_count_{};
    QLabel* observation_count_{};
    QLabel* best_result_{};
    QTableWidget* mbs_table_{};
    QTableWidget* demo_table_{};
    QLineEdit* odb_path_{};
    QComboBox* odb_target_{};
    QDoubleSpinBox* post_width_{};
    QSpinBox* post_repeat_{};
    QComboBox* acquisition_{};
    QSpinBox* initial_points_{};
    QSpinBox* random_seed_{};
    QSpinBox* candidate_pool_{};
    QDoubleSpinBox* bo_kappa_{};
    QDoubleSpinBox* bo_xi_{};
    QLabel* pending_{};
    QTabWidget* charts_{};
    QWidget* convergence_{};
    QWidget* parallel_{};
    QWidget* surrogate_{};
    QWidget* stress_strain_{};
    LogPanel* log_{};
    QString postprocess_output_path_;
    QString postprocess_sample_id_;

    void populate_table(QTableWidget* table, domain::DatasetKind dataset);
    void add_row(QTableWidget* table, domain::DatasetKind dataset);
    void delete_rows(QTableWidget* table, domain::DatasetKind dataset);
    void delete_rows_and_files(QTableWidget* table, domain::DatasetKind dataset);
    void save_table(QTableWidget* table, domain::DatasetKind dataset);
    void refresh_targets();
    void start_odb_postprocess();
    void finish_odb_postprocess(bool success);
};

class SettingsPage final : public QWidget {
  public:
    SettingsPage(ApplicationContext& context, TaskController& controller,
                 QWidget* parent = nullptr);
    void refresh();

  private:
    ApplicationContext& context_;
    TaskController& controller_;
    QLabel* database_path_{};
    QLabel* database_health_{};
    QLabel* database_counts_{};
    QLabel* runtime_status_{};
    QLabel* disk_summary_{};
    QProgressBar* disk_usage_{};
    QLineEdit* abaqus_command_{};
    QDoubleSpinBox* design_reserve_{};
    QDoubleSpinBox* simulation_reserve_{};
    LogPanel* log_{};
};

} // namespace mbs::presentation
