#pragma once

#include <QObject>
#include <QMap>
#include <QProcess>
#include <QStringList>

#include <optional>

namespace mbs::presentation {

class TaskController final : public QObject {
    Q_OBJECT

  public:
    explicit TaskController(QObject* parent = nullptr);
    void start_health_check();
    void start_worker(const QStringList& arguments, const QString& task_label);
    void stop();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString artifact_uri(const QString& key) const;
    [[nodiscard]] std::optional<double> proof_stress() const noexcept;
    [[nodiscard]] QString result_json() const;

  signals:
    void log_received(const QString& line);
    void busy_changed(bool busy);
    void event_received(const QString& event, const QString& task, const QString& message,
                        int progress_percent);
    void finished(bool success, const QString& task_label);

  private slots:
    void read_output();
    void process_finished(int exit_code, QProcess::ExitStatus status);

  private:
    QProcess process_;
    QByteArray buffer_;
    QString task_label_;
    QMap<QString, QString> artifacts_;
    std::optional<double> proof_stress_;
    QString result_json_;

    void consume_line(const QByteArray& line);
};

} // namespace mbs::presentation
