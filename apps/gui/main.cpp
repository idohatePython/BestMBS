#include "presentation/MainWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTimer>

#include <QVTKOpenGLNativeWidget.h>

#include <vtkNew.h>
#include <vtkOutputWindow.h>
#include <vtkStringOutputWindow.h>

#include <exception>

int main(int argc, char* argv[]) {
    // vtkWin32OutputWindow creates a separate native popup for reader warnings.
    // Keep diagnostics in memory; individual GUI operations report actionable
    // failures in their own log panel.
    vtkNew<vtkStringOutputWindow> vtk_output;
    vtkOutputWindow::SetInstance(vtk_output);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Mechanical Bonding Structure"));
    application.setOrganizationName(QStringLiteral("TPMS-MBS"));
    application.setApplicationVersion(QStringLiteral("4.0.0"));
    application.setStyle(QStringLiteral("Fusion"));
    try {
        mbs::presentation::MainWindow window;
        window.show();

        QString screenshot_path;
        int initial_page{};
        const auto arguments = application.arguments();
        for (int index = 1; index + 1 < arguments.size(); ++index) {
            if (arguments[index] == QStringLiteral("--screenshot")) {
                screenshot_path = arguments[index + 1];
            } else if (arguments[index] == QStringLiteral("--tab")) {
                initial_page = arguments[index + 1].toInt();
            }
        }
        window.select_page(initial_page);
        if (!screenshot_path.isEmpty()) {
            QTimer::singleShot(4000, &window, [&window, screenshot_path, &application] {
                window.grab().save(screenshot_path, "PNG");
                application.quit();
            });
        } else {
            bool valid{};
            const auto auto_close = qEnvironmentVariableIntValue("MBS_AUTOCLOSE_MS", &valid);
            if (valid && auto_close > 0) {
                QTimer::singleShot(auto_close, &application, &QApplication::quit);
            }
        }
        return application.exec();
    } catch (const std::exception& error) {
        QFile diagnostic{QDir::temp().filePath(QStringLiteral("mbs-4-startup-error.txt"))};
        if (diagnostic.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            diagnostic.write(error.what());
            diagnostic.write("\n");
        }
        QMessageBox::critical(nullptr, QStringLiteral("MBS 4.0 启动失败"),
                              QString::fromUtf8(error.what()));
        return 1;
    }
}
