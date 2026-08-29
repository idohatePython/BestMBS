#pragma once

#include "mbs/domain/DesignParameters.hpp"

#include <QWidget>

#include <array>
#include <memory>
#include <optional>

namespace mbs::presentation {

class MeshViewport final : public QWidget {
    Q_OBJECT

  public:
    explicit MeshViewport(QWidget* parent = nullptr);
    ~MeshViewport() override;

    void show_demo(const domain::DesignParameters& parameters);
    [[nodiscard]] bool load_file(const QString& path, bool render_after = true);
    [[nodiscard]] int load_directory(const QString& path);
    void clear_scene();
    void set_compact_mode(bool compact);
    void set_all_visible(bool visible);
    void set_comparison_style();
    void share_camera_from(MeshViewport* source);
    void render_linked_camera();
    [[nodiscard]] std::optional<std::array<double, 6>> scene_bounds() const;
    void set_isometric_parallel_camera(const std::array<double, 6>& bounds);

  signals:
    void scene_changed(const QString& summary);
    void message(const QString& text);
    void selection_info_changed(const QString& text);
    void camera_interacted();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mbs::presentation
