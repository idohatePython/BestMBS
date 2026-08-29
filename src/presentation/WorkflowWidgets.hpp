#pragma once

#include "mbs/domain/Material.hpp"

#include <QGroupBox>
#include <QList>
#include <QString>
#include <QWidget>

#include <memory>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace mbs::presentation {

class ApplicationContext;

class LogPanel final : public QWidget {
  public:
    explicit LogPanel(const QString& title, QWidget* parent = nullptr);

    void append(const QString& message);
    [[nodiscard]] QPlainTextEdit* editor() const noexcept;

  private:
    QPlainTextEdit* editor_{};
};

class MaterialEditor final : public QGroupBox {
  public:
    MaterialEditor(ApplicationContext& context, const QString& initial_name,
                   QWidget* parent = nullptr);
    ~MaterialEditor() override;

    [[nodiscard]] domain::MaterialDefinition material() const;
    void reload_library(const QString& preferred_name = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void load_selected();
    void new_material();
    void save_material(bool save_as);
    void rebuild_hyperelastic_fields();
    void sync_behavior_groups();
};

class FramePlayer final : public QWidget {
  public:
    explicit FramePlayer(QWidget* parent = nullptr);

    [[nodiscard]] bool load_manifest(const QString& path);

  private:
    struct Frame final {
        QString path;
        double time{};
    };

    QLabel* image_{};
    QPushButton* play_{};
    QPushButton* open_directory_{};
    QSlider* slider_{};
    QSpinBox* fps_{};
    QLabel* time_{};
    QTimer* timer_{};
    QString directory_;
    QList<Frame> frames_;

    void browse_manifest();
    void show_frame(int index);
    void toggle_playback(bool playing);
};

} // namespace mbs::presentation
