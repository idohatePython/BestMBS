#include "mbs/runtime/WorkerSession.hpp"

#include <array>
#include <stdexcept>

namespace mbs::runtime {
namespace {

bool known_event(const std::string_view event) {
    static constexpr std::array names{"started", "progress",  "artifact", "disk",
                                      "warning", "completed", "failed"};
    for (const auto* name : names) {
        if (event == name) {
            return true;
        }
    }
    return false;
}

} // namespace

WorkerSession::WorkerSession(std::string expected_task_id)
    : expected_task_id_(std::move(expected_task_id)) {
    if (expected_task_id_.empty()) {
        throw std::invalid_argument{"expected worker task id is required"};
    }
}

WorkerLine WorkerSession::consume(const std::string_view line) {
    if (!line.starts_with(event_prefix)) {
        return {.kind = WorkerLineKind::log, .text = std::string{line}, .event = std::nullopt};
    }
    auto event = EventEnvelope::decode(line);
    if (!event.has_value() || event->task_id != expected_task_id_ || !known_event(event->event)) {
        return {.kind = WorkerLineKind::rejected_event,
                .text = std::string{line},
                .event = std::nullopt};
    }
    if (event->event == "completed") {
        saw_completed_ = true;
    } else if (event->event == "failed") {
        saw_failed_ = true;
        terminal_error_ = event->message;
    }
    return {.kind = WorkerLineKind::event, .text = {}, .event = std::move(event)};
}

WorkerOutcome WorkerSession::finish(const int exit_code, const bool cancellation_requested) const {
    if (cancellation_requested) {
        return {.status = WorkerTerminalStatus::cancelled, .error = "Cancelled by user"};
    }
    if (exit_code == 0 && saw_completed_ && !saw_failed_) {
        return {.status = WorkerTerminalStatus::succeeded, .error = {}};
    }
    if (!terminal_error_.empty()) {
        return {.status = WorkerTerminalStatus::failed, .error = terminal_error_};
    }
    if (exit_code != 0) {
        return {.status = WorkerTerminalStatus::failed,
                .error = "Worker exited with code " + std::to_string(exit_code)};
    }
    return {.status = WorkerTerminalStatus::failed,
            .error = "Worker exited without a completion event"};
}

bool WorkerSession::saw_terminal_event() const noexcept { return saw_completed_ || saw_failed_; }

} // namespace mbs::runtime
