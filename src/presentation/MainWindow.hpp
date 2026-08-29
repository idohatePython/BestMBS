#pragma once

#include "presentation/ApplicationContext.hpp"
#include "presentation/TaskController.hpp"

#include <QMainWindow>

class QCloseEvent;
class QLabel;
class QTabWidget;

namespace mbs::presentation {

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    void select_page(int index);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    ApplicationContext context_;
    TaskController controller_;
    QTabWidget* tabs_{};
    QLabel* task_status_{};
};

} // namespace mbs::presentation
