#include "presentation/MeshViewport.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeSource.h>
#include <vtkDataObject.h>
#include <vtkDataSet.h>
#include <vtkDataSetSurfaceFilter.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGenericDataObjectReader.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkMapper.h>
#include <vtkNew.h>
#include <vtkOBJReader.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPlane.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropPicker.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSTLReader.h>
#include <vtkSmartPointer.h>
#include <vtkUnstructuredGrid.h>
#include <vtkXMLPolyDataReader.h>
#include <vtkXMLImageDataReader.h>
#include <vtkXMLUnstructuredGridReader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace mbs::presentation {
namespace {

constexpr double pi = 3.14159265358979323846;

struct ObjectStyle final {
    QString label;
    QColor color{142, 154, 175};
    double opacity{0.9};
    bool edges{};
    bool visible{};
    bool wireframe{};
};

ObjectStyle object_style(const QString& key, const std::size_t fallback_index) {
    static const std::array fallback_colors{QColor{38, 198, 218}, QColor{255, 112, 67},
                                            QColor{126, 87, 194}, QColor{102, 187, 106}};
    ObjectStyle style{.label = key,
                      .color = fallback_colors[fallback_index % fallback_colors.size()]};
    if (key == QStringLiteral("grid"))
        return {QObject::tr("1. 三维规则采样网格 grid"), QColor{96, 125, 139}, 0.45, true,
                false, true};
    if (key == QStringLiteral("prt_a"))
        return {QObject::tr("2. 布尔原网格 Part A"), QColor{25, 118, 210}, 0.9, false, false,
                false};
    if (key == QStringLiteral("prt_b"))
        return {QObject::tr("3. 布尔原网格 Part B"), QColor{239, 108, 0}, 0.9, false, false,
                false};
    if (key == QStringLiteral("prt_a_rms"))
        return {QObject::tr("4. 重网格 Part A"), QColor{13, 71, 161}, 1.0, true, true, false};
    if (key == QStringLiteral("prt_b_rms"))
        return {QObject::tr("5. 重网格 Part B"), QColor{230, 81, 0}, 1.0, true, true, false};
    if (key == QStringLiteral("prt_a_grid"))
        return {QObject::tr("6. TetGen Part A"), QColor{66, 165, 245}, 1.0, true, false,
                false};
    if (key == QStringLiteral("prt_b_grid"))
        return {QObject::tr("7. TetGen Part B"), QColor{255, 183, 77}, 1.0, true, false,
                false};
    if (key == QStringLiteral("prt_a_subgrid"))
        return {QObject::tr("8. TetGen Part A 截取"), QColor{144, 202, 249}, 1.0, true,
                false, false};
    if (key == QStringLiteral("prt_b_subgrid"))
        return {QObject::tr("9. TetGen Part B 截取"), QColor{255, 204, 128}, 1.0, true,
                false, false};
    return style;
}

vtkSmartPointer<vtkPolyData> as_poly_data(vtkDataObject* data) {
    auto result = vtkSmartPointer<vtkPolyData>::New();
    if (auto* poly_data = vtkPolyData::SafeDownCast(data)) {
        result->DeepCopy(poly_data);
        return result;
    }
    if (auto* data_set = vtkDataSet::SafeDownCast(data)) {
        vtkNew<vtkDataSetSurfaceFilter> surface;
        surface->SetInputData(data_set);
        surface->Update();
        result->DeepCopy(surface->GetOutput());
    }
    return result;
}

vtkSmartPointer<vtkPolyData> read_mesh(const QString& path) {
    const auto suffix = QFileInfo{path}.suffix().toLower();
    const auto native_path = path.toUtf8();
    if (suffix == QStringLiteral("stl")) {
        vtkNew<vtkSTLReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    if (suffix == QStringLiteral("obj")) {
        vtkNew<vtkOBJReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    if (suffix == QStringLiteral("vtp")) {
        vtkNew<vtkXMLPolyDataReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    if (suffix == QStringLiteral("vtu")) {
        vtkNew<vtkXMLUnstructuredGridReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    if (suffix == QStringLiteral("vti")) {
        vtkNew<vtkXMLImageDataReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    if (suffix == QStringLiteral("vtk")) {
        vtkNew<vtkGenericDataObjectReader> reader;
        reader->SetFileName(native_path.constData());
        reader->Update();
        return as_poly_data(reader->GetOutput());
    }
    return vtkSmartPointer<vtkPolyData>::New();
}

vtkSmartPointer<vtkPolyData> gyroid_surface(const domain::DesignParameters& parameters,
                                            const double contour_value) {
    constexpr int resolution = 52;
    vtkNew<vtkImageData> field;
    field->SetDimensions(resolution, resolution, resolution);
    const auto spacing = 2.0 * pi / static_cast<double>(resolution - 1);
    field->SetOrigin(-pi, -pi, -pi);
    field->SetSpacing(spacing, spacing, spacing);
    field->AllocateScalars(VTK_DOUBLE, 1);

    const auto phase_x = 2.0 * pi * parameters.phase_x;
    const auto phase_y = 2.0 * pi * parameters.phase_y;
    const auto phase_z = pi * parameters.beta;
    const auto anisotropy = 0.72 + 0.5 * parameters.kappa;
    for (int z = 0; z < resolution; ++z) {
        const auto z_value = -pi + spacing * static_cast<double>(z);
        for (int y = 0; y < resolution; ++y) {
            const auto y_value = -pi + spacing * static_cast<double>(y);
            for (int x = 0; x < resolution; ++x) {
                const auto x_value = -pi + spacing * static_cast<double>(x);
                const auto gyroid =
                    std::sin(x_value + phase_x) * std::cos(y_value + phase_y) +
                    std::sin(y_value + phase_y) * std::cos(anisotropy * z_value + phase_z) +
                    std::sin(anisotropy * z_value + phase_z) * std::cos(x_value + phase_x);
                const auto blend = parameters.lambda * std::cos(2.0 * x_value) +
                                   parameters.mu * std::sin(2.0 * y_value);
                auto* scalar = static_cast<double*>(field->GetScalarPointer(x, y, z));
                *scalar = gyroid + 0.18 * blend;
            }
        }
    }

    vtkNew<vtkFlyingEdges3D> contour;
    contour->SetInputData(field);
    contour->SetValue(0, contour_value);
    contour->ComputeNormalsOn();
    contour->Update();
    auto surface = vtkSmartPointer<vtkPolyData>::New();
    surface->DeepCopy(contour->GetOutput());
    return surface;
}

} // namespace

struct MeshViewport::Impl final {
    struct SceneObject final {
        QString key;
        QString name;
        vtkSmartPointer<vtkPolyData> mesh;
        vtkSmartPointer<vtkActor> actor;
    };

    MeshViewport* owner{};
    QVTKOpenGLNativeWidget* widget{};
    QWidget* toolbar_widget{};
    QWidget* side_widget{};
    QListWidget* objects{};
    QLabel* statistics{};
    QCheckBox* edges{};
    QCheckBox* parallel_projection{};
    QCheckBox* invert_clip{};
    QPushButton* color_button{};
    QPushButton* clip_button{};
    QPushButton* rotate_button{};
    QSlider* opacity_slider{};
    QLabel* opacity_value{};
    QLabel* color_swatch{};
    QTimer* rotation_timer{};
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> render_window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkOrientationMarkerWidget> orientation_marker;
    vtkSmartPointer<vtkPlane> clip_plane;
    vtkSmartPointer<vtkPropPicker> picker;
    vtkSmartPointer<vtkCallbackCommand> interaction_callback;
    std::vector<unsigned long> interaction_observers;
    std::vector<SceneObject> scene;
    bool batch_loading{};

    [[nodiscard]] SceneObject* selected_object() {
        const auto row = objects->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= scene.size()) {
            return nullptr;
        }
        return &scene[static_cast<std::size_t>(row)];
    }

    void sync_appearance() {
        const auto* object = selected_object();
        const auto enabled = object != nullptr;
        color_button->setEnabled(enabled);
        opacity_slider->setEnabled(enabled);
        edges->setEnabled(enabled);
        if (!enabled) {
            return;
        }
        edges->blockSignals(true);
        opacity_slider->blockSignals(true);
        edges->setChecked(object->actor->GetProperty()->GetEdgeVisibility() != 0);
        opacity_slider->setValue(static_cast<int>(std::lround(
            object->actor->GetProperty()->GetOpacity() * 100.0)));
        opacity_value->setText(QStringLiteral("%1%").arg(opacity_slider->value()));
        const auto color = object->actor->GetProperty()->GetColor();
        const QColor qt_color = QColor::fromRgbF(static_cast<float>(color[0]),
                                                 static_cast<float>(color[1]),
                                                 static_cast<float>(color[2]));
        color_swatch->setStyleSheet(
            QStringLiteral("background:%1;border:1px solid #78909c;border-radius:3px")
                .arg(qt_color.name()));
        edges->blockSignals(false);
        opacity_slider->blockSignals(false);
        update_selection_info();
    }

    void update_selection_info() {
        const auto* object = selected_object();
        if (object == nullptr) {
            emit owner->selection_info_changed(QObject::tr("尚未选择可视对象。"));
            return;
        }
        double bounds[6]{};
        object->mesh->GetBounds(bounds);
        emit owner->selection_info_changed(
            QObject::tr("%1\n顶点：%2\n单元：%3\n范围 X[%4, %5]  Y[%6, %7]  Z[%8, %9]")
                .arg(object->name)
                .arg(object->mesh->GetNumberOfPoints())
                .arg(object->mesh->GetNumberOfCells())
                .arg(bounds[0], 0, 'g', 6)
                .arg(bounds[1], 0, 'g', 6)
                .arg(bounds[2], 0, 'g', 6)
                .arg(bounds[3], 0, 'g', 6)
                .arg(bounds[4], 0, 'g', 6)
                .arg(bounds[5], 0, 'g', 6));
    }

    void apply_clip() {
        const auto enabled = clip_button->isChecked();
        const auto normal = invert_clip->isChecked() ? -1.0 : 1.0;
        clip_plane->SetNormal(0.0, normal, 0.0);
        const auto object_checked = [this](const std::size_t index) {
            const auto* item = objects->item(static_cast<int>(index));
            return item != nullptr && item->checkState() == Qt::Checked;
        };
        const auto find_key = [this](const QString& key) -> std::optional<std::size_t> {
            for (std::size_t index = 0; index < scene.size(); ++index) {
                if (scene[index].key == key) {
                    return index;
                }
            }
            return std::nullopt;
        };
        for (std::size_t index = 0; index < scene.size(); ++index) {
            const auto& object = scene[index];
            auto* mapper = object.actor->GetMapper();
            mapper->RemoveAllClippingPlanes();
            const auto is_full_tet = object.key == QStringLiteral("prt_a_grid") ||
                                     object.key == QStringLiteral("prt_b_grid");
            const auto is_subgrid = object.key == QStringLiteral("prt_a_subgrid") ||
                                    object.key == QStringLiteral("prt_b_subgrid");
            if (enabled && is_full_tet) {
                const auto counterpart = object.key == QStringLiteral("prt_a_grid")
                                             ? QStringLiteral("prt_a_subgrid")
                                             : QStringLiteral("prt_b_subgrid");
                if (find_key(counterpart)) {
                    object.actor->SetVisibility(false);
                    continue;
                }
            }
            if (enabled && is_subgrid) {
                const auto full_key = object.key == QStringLiteral("prt_a_subgrid")
                                          ? QStringLiteral("prt_a_grid")
                                          : QStringLiteral("prt_b_grid");
                const auto full = find_key(full_key);
                object.actor->SetVisibility(object_checked(index) ||
                                            (full && object_checked(*full)));
                continue;
            }
            object.actor->SetVisibility(object_checked(index));
            if (enabled && object.actor->GetVisibility()) {
                mapper->AddClippingPlane(clip_plane);
            }
        }
        render();
    }

    void add_object(QString key, QString name, vtkSmartPointer<vtkPolyData> mesh,
                    const ObjectStyle& style) {
        if (mesh == nullptr || mesh->GetNumberOfPoints() == 0) {
            return;
        }
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(mesh);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(style.color.redF(), style.color.greenF(),
                                       style.color.blueF());
        actor->GetProperty()->SetOpacity(style.opacity);
        actor->GetProperty()->SetInterpolationToPhong();
        actor->GetProperty()->SetSpecular(0.25);
        actor->GetProperty()->SetSpecularPower(22.0);
        actor->GetProperty()->SetEdgeVisibility(style.edges);
        if (style.wireframe) {
            actor->GetProperty()->SetRepresentationToWireframe();
        }
        actor->SetVisibility(style.visible);
        renderer->AddActor(actor);
        scene.push_back({std::move(key), name, std::move(mesh), actor});

        auto* item = new QListWidgetItem(std::move(name), objects);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(style.visible ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::DecorationRole, style.color);
        if (objects->currentRow() < 0) {
            objects->setCurrentItem(item);
        }
        if (!batch_loading) {
            update_statistics();
            update_selection_info();
        }
    }

    void update_statistics() {
        vtkIdType points{};
        vtkIdType cells{};
        for (const auto& object : scene) {
            points += object.mesh->GetNumberOfPoints();
            cells += object.mesh->GetNumberOfCells();
        }
        const auto text =
            QObject::tr("对象 %1  ·  顶点 %2  ·  单元 %3").arg(scene.size()).arg(points).arg(cells);
        statistics->setText(text);
        emit owner->scene_changed(text);
    }

    void render(const bool reset_camera = false) {
        if (reset_camera) {
            renderer->ResetCamera();
            renderer->ResetCameraClippingRange();
        }
        render_window->Render();
    }
};

MeshViewport::MeshViewport(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    impl_->toolbar_widget = new QWidget(this);
    auto* toolbar = new QHBoxLayout(impl_->toolbar_widget);
    toolbar->setContentsMargins(0, 0, 0, 0);
    const auto make_button = [this, toolbar](const QString& text, const auto& callback) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(button, &QToolButton::clicked, this, callback);
        toolbar->addWidget(button);
    };
    make_button(tr("载入网格"), [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, tr("载入三维网格"), {},
            tr("三维网格 (*.stl *.obj *.vtk *.vtp *.vtu);;所有文件 (*)"));
        if (!path.isEmpty()) {
            static_cast<void>(load_file(path));
        }
    });
    make_button(tr("等轴测"), [this] {
        impl_->renderer->GetActiveCamera()->SetPosition(1.0, 1.0, 1.0);
        impl_->renderer->GetActiveCamera()->SetFocalPoint(0.0, 0.0, 0.0);
        impl_->renderer->GetActiveCamera()->SetViewUp(0.0, 0.0, 1.0);
        impl_->renderer->GetActiveCamera()->OrthogonalizeViewUp();
        impl_->render(true);
    });
    make_button(tr("前视"), [this] {
        impl_->renderer->GetActiveCamera()->SetPosition(0.0, -1.0, 0.0);
        impl_->renderer->GetActiveCamera()->SetFocalPoint(0.0, 0.0, 0.0);
        impl_->renderer->GetActiveCamera()->SetViewUp(0.0, 0.0, 1.0);
        impl_->renderer->GetActiveCamera()->OrthogonalizeViewUp();
        impl_->render(true);
    });
    make_button(tr("俯视"), [this] {
        impl_->renderer->GetActiveCamera()->SetPosition(0.0, 0.0, 1.0);
        impl_->renderer->GetActiveCamera()->SetFocalPoint(0.0, 0.0, 0.0);
        impl_->renderer->GetActiveCamera()->SetViewUp(0.0, 1.0, 0.0);
        impl_->renderer->GetActiveCamera()->OrthogonalizeViewUp();
        impl_->render(true);
    });
    make_button(tr("相机复位"), [this] { impl_->render(true); });
    make_button(tr("截图"), [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, tr("保存视口截图"), QStringLiteral("tpms-view.png"), tr("PNG 图片 (*.png)"));
        if (!path.isEmpty() && impl_->widget->grab().save(path, "PNG")) {
            emit message(tr("视口截图已保存：%1").arg(path));
        }
    });

    impl_->parallel_projection = new QCheckBox(tr("正交投影"), this);
    connect(impl_->parallel_projection, &QCheckBox::toggled, this, [this](const bool checked) {
        impl_->renderer->GetActiveCamera()->SetParallelProjection(checked);
        impl_->render();
    });
    toolbar->addWidget(impl_->parallel_projection);
    toolbar->addStretch(1);
    root->addWidget(impl_->toolbar_widget);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    impl_->widget = new QVTKOpenGLNativeWidget(splitter);
    impl_->render_window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    impl_->renderer = vtkSmartPointer<vtkRenderer>::New();
    impl_->clip_plane = vtkSmartPointer<vtkPlane>::New();
    impl_->clip_plane->SetOrigin(0.0, 0.0, 0.0);
    impl_->clip_plane->SetNormal(0.0, 1.0, 0.0);
    impl_->picker = vtkSmartPointer<vtkPropPicker>::New();
    impl_->renderer->SetGradientBackground(false);
    impl_->renderer->SetBackground(0.961, 0.969, 0.980);
    impl_->render_window->AddRenderer(impl_->renderer);
    impl_->render_window->SetMultiSamples(4);
    impl_->widget->setRenderWindow(impl_->render_window);
    impl_->interaction_callback = vtkSmartPointer<vtkCallbackCommand>::New();
    impl_->interaction_callback->SetClientData(this);
    impl_->interaction_callback->SetCallback(
        [](vtkObject*, const unsigned long event, void* client_data, void*) {
            auto* viewport = static_cast<MeshViewport*>(client_data);
            if (event == vtkCommand::LeftButtonPressEvent) {
                auto* impl = viewport->impl_.get();
                const auto* position = impl->widget->interactor()->GetEventPosition();
                if (impl->picker->PickProp(position[0], position[1], impl->renderer) != 0) {
                    auto* actor = impl->picker->GetActor();
                    for (std::size_t index = 0; index < impl->scene.size(); ++index) {
                        if (impl->scene[index].actor == actor) {
                            impl->objects->setCurrentRow(static_cast<int>(index));
                            break;
                        }
                    }
                }
            }
            emit viewport->camera_interacted();
        });
    for (const auto event : {vtkCommand::InteractionEvent, vtkCommand::EndInteractionEvent,
                             vtkCommand::MouseMoveEvent, vtkCommand::MouseWheelForwardEvent,
                             vtkCommand::MouseWheelBackwardEvent,
                             vtkCommand::LeftButtonPressEvent}) {
        impl_->interaction_observers.push_back(
            impl_->widget->interactor()->AddObserver(event, impl_->interaction_callback));
    }
    splitter->addWidget(impl_->widget);

    auto* side = new QWidget(splitter);
    impl_->side_widget = side;
    side->setMinimumWidth(220);
    side->setMaximumWidth(300);
    auto* side_layout = new QVBoxLayout(side);
    auto* title = new QLabel(tr("可视对象（可多选）"), side);
    title->setObjectName(QStringLiteral("sectionTitle"));
    side_layout->addWidget(title);
    impl_->objects = new QListWidget(side);
    side_layout->addWidget(impl_->objects, 1);

    auto* appearance = new QGroupBox(tr("对象外观"), side);
    auto* appearance_layout = new QVBoxLayout(appearance);
    impl_->edges = new QCheckBox(tr("显示边线"), appearance);
    impl_->edges->setEnabled(false);
    connect(impl_->edges, &QCheckBox::toggled, this, [this](const bool checked) {
        if (auto* object = impl_->selected_object()) {
            object->actor->GetProperty()->SetEdgeVisibility(checked);
            impl_->render();
        }
    });
    appearance_layout->addWidget(impl_->edges);
    auto* color_row = new QHBoxLayout;
    impl_->color_button = new QPushButton(tr("选择颜色"), appearance);
    impl_->color_button->setEnabled(false);
    impl_->color_swatch = new QLabel(appearance);
    impl_->color_swatch->setFixedSize(28, 22);
    connect(impl_->color_button, &QPushButton::clicked, this, [this] {
        auto* object = impl_->selected_object();
        if (object == nullptr) {
            return;
        }
        const auto current = object->actor->GetProperty()->GetColor();
        const auto selected = QColorDialog::getColor(
            QColor::fromRgbF(static_cast<float>(current[0]), static_cast<float>(current[1]),
                             static_cast<float>(current[2])),
            this, tr("选择对象颜色"));
        if (!selected.isValid()) {
            return;
        }
        object->actor->GetProperty()->SetColor(selected.redF(), selected.greenF(), selected.blueF());
        impl_->sync_appearance();
        impl_->render();
    });
    color_row->addWidget(impl_->color_button);
    color_row->addWidget(impl_->color_swatch);
    color_row->addStretch(1);
    appearance_layout->addLayout(color_row);
    auto* opacity_row = new QHBoxLayout;
    opacity_row->addWidget(new QLabel(tr("透明度"), appearance));
    impl_->opacity_slider = new QSlider(Qt::Horizontal, appearance);
    impl_->opacity_slider->setRange(0, 100);
    impl_->opacity_slider->setValue(90);
    impl_->opacity_slider->setEnabled(false);
    impl_->opacity_value = new QLabel(QStringLiteral("90%"), appearance);
    connect(impl_->opacity_slider, &QSlider::valueChanged, this, [this](const int value) {
        impl_->opacity_value->setText(QStringLiteral("%1%").arg(value));
        if (auto* object = impl_->selected_object()) {
            object->actor->GetProperty()->SetOpacity(static_cast<double>(value) / 100.0);
            impl_->render();
        }
    });
    opacity_row->addWidget(impl_->opacity_slider, 1);
    opacity_row->addWidget(impl_->opacity_value);
    appearance_layout->addLayout(opacity_row);
    side_layout->addWidget(appearance);

    auto* scene_controls = new QGridLayout;
    impl_->clip_button = new QPushButton(tr("开启截面"), side);
    impl_->clip_button->setCheckable(true);
    impl_->rotate_button = new QPushButton(tr("三维展示"), side);
    impl_->rotate_button->setCheckable(true);
    impl_->invert_clip = new QCheckBox(tr("反向截取"), side);
    scene_controls->addWidget(impl_->clip_button, 0, 0);
    scene_controls->addWidget(impl_->rotate_button, 0, 1);
    scene_controls->addWidget(impl_->invert_clip, 1, 0, 1, 2);
    side_layout->addLayout(scene_controls);
    connect(impl_->clip_button, &QPushButton::toggled, this, [this](const bool enabled) {
        impl_->clip_button->setText(enabled ? tr("关闭截面") : tr("开启截面"));
        impl_->apply_clip();
    });
    connect(impl_->invert_clip, &QCheckBox::toggled, this,
            [this] { impl_->apply_clip(); });
    impl_->rotation_timer = new QTimer(this);
    impl_->rotation_timer->setInterval(40);
    connect(impl_->rotation_timer, &QTimer::timeout, this, [this] {
        impl_->renderer->GetActiveCamera()->Azimuth(2.0);
        impl_->render();
    });
    connect(impl_->rotate_button, &QPushButton::toggled, this, [this](const bool enabled) {
        impl_->rotate_button->setText(enabled ? tr("停止展示") : tr("三维展示"));
        if (enabled) {
            impl_->rotation_timer->start();
        } else {
            impl_->rotation_timer->stop();
        }
    });
    impl_->statistics = new QLabel(side);
    impl_->statistics->setWordWrap(true);
    impl_->statistics->setObjectName(QStringLiteral("mutedLabel"));
    side_layout->addWidget(impl_->statistics);
    auto* hint = new QLabel(
        tr("单击模型可选中对象 · 左键拖动旋转 · 中键平移 · 滚轮缩放\n列表勾选控制对象显隐"),
        side);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("mutedLabel"));
    side_layout->addWidget(hint);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    root->addWidget(splitter, 1);

    connect(impl_->objects, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const auto row = impl_->objects->row(item);
        if (row >= 0 && static_cast<std::size_t>(row) < impl_->scene.size()) {
            impl_->apply_clip();
        }
    });
    connect(impl_->objects, &QListWidget::currentRowChanged, this,
            [this] { impl_->sync_appearance(); });

    vtkNew<vtkAxesActor> axes;
    impl_->orientation_marker = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    impl_->orientation_marker->SetOrientationMarker(axes);
    impl_->orientation_marker->SetInteractor(impl_->widget->interactor());
    impl_->orientation_marker->SetViewport(0.0, 0.0, 0.16, 0.22);
    impl_->orientation_marker->SetEnabled(1);
    impl_->orientation_marker->InteractiveOff();
    // Creating an OpenGL context before the native Qt window is exposed can
    // block MinGW/Windows drivers. Defer the first VTK render to the event loop.
    QTimer::singleShot(0, this, [this] {
        impl_->render();
        emit selection_info_changed(tr("尚未选择可视对象。\n完成设计或加载已有模型后，此处将显示网格信息。"));
    });
}

MeshViewport::~MeshViewport() = default;

void MeshViewport::set_compact_mode(const bool compact) {
    impl_->toolbar_widget->setVisible(!compact);
    impl_->side_widget->setVisible(!compact);
}

void MeshViewport::set_all_visible(const bool visible) {
    impl_->objects->blockSignals(true);
    for (std::size_t index = 0; index < impl_->scene.size(); ++index) {
        impl_->scene[index].actor->SetVisibility(visible);
        if (auto* item = impl_->objects->item(static_cast<int>(index))) {
            item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
        }
    }
    impl_->objects->blockSignals(false);
    impl_->renderer->ResetCamera();
    impl_->renderer->ResetCameraClippingRange();
    impl_->apply_clip();
}

void MeshViewport::set_comparison_style() {
    for (auto& object : impl_->scene) {
        auto* property = object.actor->GetProperty();
        property->SetOpacity(1.0);
        if (object.mesh->GetNumberOfPolys() > 0 || object.mesh->GetNumberOfStrips() > 0) {
            property->SetRepresentationToSurface();
            property->SetEdgeVisibility(1);
            property->SetEdgeColor(0.08, 0.08, 0.08);
            property->SetLineWidth(1.0);
        }
    }
    impl_->render();
}

void MeshViewport::share_camera_from(MeshViewport* source) {
    if (source == nullptr || source == this) {
        return;
    }
    impl_->renderer->SetActiveCamera(source->impl_->renderer->GetActiveCamera());
    impl_->render();
}

void MeshViewport::render_linked_camera() {
    impl_->renderer->ResetCameraClippingRange();
    impl_->render();
}

std::optional<std::array<double, 6>> MeshViewport::scene_bounds() const {
    if (impl_->scene.empty()) {
        return std::nullopt;
    }
    std::array<double, 6> result{std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()};
    for (const auto& object : impl_->scene) {
        double bounds[6]{};
        object.mesh->GetBounds(bounds);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result[axis * 2] = std::min(result[axis * 2], bounds[axis * 2]);
            result[axis * 2 + 1] = std::max(result[axis * 2 + 1], bounds[axis * 2 + 1]);
        }
    }
    return result;
}

void MeshViewport::set_isometric_parallel_camera(const std::array<double, 6>& bounds) {
    const std::array center{(bounds[0] + bounds[1]) * 0.5,
                            (bounds[2] + bounds[3]) * 0.5,
                            (bounds[4] + bounds[5]) * 0.5};
    auto* camera = impl_->renderer->GetActiveCamera();
    camera->SetFocalPoint(center.data());
    camera->SetPosition(center[0] + 1.0, center[1] + 1.0, center[2] + 1.0);
    camera->SetViewUp(0.0, 0.0, 1.0);
    camera->OrthogonalizeViewUp();
    camera->SetParallelProjection(true);
    impl_->renderer->ResetCamera(bounds.data());
    impl_->renderer->ResetCameraClippingRange(bounds.data());
    impl_->render();
}

void MeshViewport::show_demo(const domain::DesignParameters& parameters) {
    clear_scene();
    const auto level = 0.28 + 0.2 * parameters.kappa;
    impl_->add_object(QStringLiteral("preview_a"), tr("互锁相 A · Gyroid +"),
                      gyroid_surface(parameters, level),
                      {.label = {}, .color = QColor{33, 150, 243}, .opacity = 0.92,
                       .visible = true});
    impl_->add_object(QStringLiteral("preview_b"), tr("互锁相 B · Gyroid −"),
                      gyroid_surface(parameters, -level),
                      {.label = {}, .color = QColor{255, 167, 38}, .opacity = 0.78,
                       .visible = true});

    vtkNew<vtkCubeSource> lower_plate;
    lower_plate->SetCenter(0.0, 0.0, -3.35);
    lower_plate->SetXLength(6.6);
    lower_plate->SetYLength(6.6);
    lower_plate->SetZLength(0.28);
    lower_plate->Update();
    impl_->add_object(QStringLiteral("preview_lower"), tr("下盖板"),
                      as_poly_data(lower_plate->GetOutput()),
                      {.label = {}, .color = QColor{117, 125, 135}, .opacity = 0.75,
                       .visible = true});
    vtkNew<vtkCubeSource> upper_plate;
    upper_plate->SetCenter(0.0, 0.0, 3.35);
    upper_plate->SetXLength(6.6);
    upper_plate->SetYLength(6.6);
    upper_plate->SetZLength(0.28);
    upper_plate->Update();
    impl_->add_object(QStringLiteral("preview_upper"), tr("上盖板"),
                      as_poly_data(upper_plate->GetOutput()),
                      {.label = {}, .color = QColor{117, 125, 135}, .opacity = 0.75,
                       .visible = true});
    impl_->render(true);
    emit message(tr("已由 C++/VTK 实时生成双相 Gyroid 预览"));
}

bool MeshViewport::load_file(const QString& path, const bool render_after) {
    auto mesh = read_mesh(path);
    if (mesh == nullptr || mesh->GetNumberOfPoints() == 0) {
        emit message(tr("无法读取网格：%1").arg(path));
        return false;
    }
    const auto key = QFileInfo{path}.completeBaseName();
    const auto style = object_style(key, impl_->scene.size());
    impl_->add_object(key, style.label == key ? QFileInfo{path}.fileName() : style.label,
                      std::move(mesh), style);
    if (render_after) {
        impl_->render(true);
        emit message(tr("已载入：%1").arg(path));
    }
    return true;
}

int MeshViewport::load_directory(const QString& path) {
    int loaded{};
    impl_->batch_loading = true;
    const QDir directory{path};
    const QStringList filters{QStringLiteral("*.stl"), QStringLiteral("*.obj"),
                              QStringLiteral("*.vtk"), QStringLiteral("*.vti"),
                              QStringLiteral("*.vtp"), QStringLiteral("*.vtu")};
    const QStringList order{QStringLiteral("grid"),          QStringLiteral("prt_a"),
                            QStringLiteral("prt_b"),         QStringLiteral("prt_a_rms"),
                            QStringLiteral("prt_b_rms"),     QStringLiteral("prt_a_grid"),
                            QStringLiteral("prt_b_grid"),    QStringLiteral("prt_a_subgrid"),
                            QStringLiteral("prt_b_subgrid"), QStringLiteral("tl_a"),
                            QStringLiteral("tl_b"),
                            QStringLiteral("tl_c"),          QStringLiteral("plt_a"),
                            QStringLiteral("plt_b"),         QStringLiteral("prt_a0"),
                            QStringLiteral("prt_b0"),        QStringLiteral("prt_a1"),
                            QStringLiteral("prt_b1")};
    auto files = directory.entryInfoList(filters, QDir::Files, QDir::Name);
    std::stable_sort(files.begin(), files.end(), [&order](const QFileInfo& left,
                                                          const QFileInfo& right) {
        const auto left_index = order.indexOf(left.completeBaseName());
        const auto right_index = order.indexOf(right.completeBaseName());
        const auto normalized_left = left_index < 0 ? order.size() : left_index;
        const auto normalized_right = right_index < 0 ? order.size() : right_index;
        return normalized_left == normalized_right ? left.fileName() < right.fileName()
                                                   : normalized_left < normalized_right;
    });
    for (const auto& file : files) {
        const auto key = file.completeBaseName();
        if (key == QStringLiteral("tpms_mc") || key == QStringLiteral("tpms") ||
            key == QStringLiteral("tpms_sole")) {
            continue;
        }
        if (load_file(file.absoluteFilePath(), false)) {
            ++loaded;
        }
    }
    impl_->batch_loading = false;
    impl_->update_statistics();
    impl_->update_selection_info();
    if (loaded > 0) {
        impl_->render(true);
    }
    return loaded;
}

void MeshViewport::clear_scene() {
    impl_->renderer->RemoveAllViewProps();
    impl_->scene.clear();
    impl_->objects->clear();
    impl_->rotation_timer->stop();
    impl_->rotate_button->setChecked(false);
    impl_->update_statistics();
    impl_->update_selection_info();
}

} // namespace mbs::presentation
