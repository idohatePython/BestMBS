#include "presentation/TaskController.hpp"

#include "mbs/runtime/EventEnvelope.hpp"

#include <QCoreApplication>
#include <QDir>

#include <cmath>

namespace mbs::presentation {

TaskController::TaskController(QObject* parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &TaskController::read_output);
    connect(&process_, &QProcess::finished, this, &TaskController::process_finished);
}

void TaskController::start_health_check() { start_worker({}, QStringLiteral("Worker 健康检查")); }

void TaskController::start_worker(const QStringList& arguments, const QString& task_label) {
    if (process_.state() != QProcess::NotRunning) {
        return;
    }
    const auto executable = QDir{QCoreApplication::applicationDirPath()}.filePath("mbs-worker.exe");
    artifacts_.clear();
    proof_stress_.reset();
    result_json_.clear();
    task_label_ = task_label;
    process_.setProgram(executable);
    process_.setArguments(arguments);
    process_.start();
    emit busy_changed(true);
    emit log_received(QStringLiteral("▶ %1").arg(task_label_));
}

void TaskController::stop() {
    if (process_.state() == QProcess::NotRunning) {
        return;
    }
    process_.terminate();
    if (!process_.waitForFinished(1500)) {
        process_.kill();
    }
}

bool TaskController::busy() const noexcept { return process_.state() != QProcess::NotRunning; }

QString TaskController::artifact_uri(const QString& key) const { return artifacts_.value(key); }

std::optional<double> TaskController::proof_stress() const noexcept { return proof_stress_; }

QString TaskController::result_json() const { return result_json_; }

void TaskController::read_output() {
    buffer_ += process_.readAllStandardOutput();
    while (true) {
        const auto newline = buffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const auto line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) {
            consume_line(line);
        }
    }
}

void TaskController::consume_line(const QByteArray& line) {
    const auto text = QString::fromUtf8(line);
    const auto event = runtime::EventEnvelope::decode(text.toStdString());
    if (!event.has_value()) {
        emit log_received(text);
        return;
    }
    const auto progress =
        event->progress.has_value() ? static_cast<int>(std::lround(*event->progress * 100.0)) : -1;
    for (const auto& [key, value] : event->artifact_uris) {
        artifacts_.insert(QString::fromStdString(key), QString::fromStdString(value));
    }
    if (event->proof_stress.has_value()) {
        proof_stress_ = event->proof_stress;
    }
    if (!event->result_json.empty()) {
        result_json_ = QString::fromStdString(event->result_json);
    }
    emit event_received(QString::fromStdString(event->event),
                        QString::fromStdString(event->task_kind),
                        QString::fromStdString(event->message), progress);
    emit log_received(QStringLiteral("[%1] %2").arg(QString::fromStdString(event->event),
                                                    QString::fromStdString(event->message)));
}

void TaskController::process_finished(const int exit_code, const QProcess::ExitStatus status) {
    if (!buffer_.trimmed().isEmpty()) {
        consume_line(buffer_.trimmed());
    }
    buffer_.clear();
    const auto completed_label = task_label_;
    task_label_.clear();
    emit log_received(QStringLiteral("Worker exit: code=%1, status=%2")
                          .arg(exit_code)
                          .arg(status == QProcess::NormalExit ? QStringLiteral("normal")
                                                              : QStringLiteral("crashed")));
    emit busy_changed(false);
    emit finished(exit_code == 0 && status == QProcess::NormalExit, completed_label);
}

} // namespace mbs::presentation
