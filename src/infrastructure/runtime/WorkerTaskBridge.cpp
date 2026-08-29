#include "mbs/infrastructure/runtime/WorkerTaskBridge.hpp"

#include <stdexcept>
#include <utility>

namespace mbs::infrastructure::runtime {
namespace {

application::WorkerEventKind event_kind(const std::string_view event) {
    if (event == "artifact") {
        return application::WorkerEventKind::artifact;
    }
    if (event == "completed") {
        return application::WorkerEventKind::completed;
    }
    return application::WorkerEventKind::progress;
}

domain::TaskStatus terminal_status(const mbs::runtime::WorkerTerminalStatus status) {
    switch (status) {
    case mbs::runtime::WorkerTerminalStatus::succeeded:
        return domain::TaskStatus::succeeded;
    case mbs::runtime::WorkerTerminalStatus::failed:
        return domain::TaskStatus::failed;
    case mbs::runtime::WorkerTerminalStatus::cancelled:
        return domain::TaskStatus::cancelled;
    }
    return domain::TaskStatus::failed;
}

} // namespace

WorkerTaskBridge::WorkerTaskBridge(application::TaskLifecycleService& lifecycle,
                                   application::TaskExecution execution,
                                   std::optional<std::string> configured_sample_id)
    : lifecycle_(lifecycle), execution_(std::move(execution)),
      configured_sample_id_(std::move(configured_sample_id)), session_(execution_.task_id) {
    if (execution_.run_id.empty()) {
        throw std::invalid_argument{"worker bridge run id is required"};
    }
}

mbs::runtime::WorkerLine WorkerTaskBridge::consume(const std::string_view line) {
    if (finished_) {
        throw std::logic_error{"cannot consume output after worker task has finished"};
    }
    auto parsed = session_.consume(line);
    if (parsed.kind != mbs::runtime::WorkerLineKind::event || !parsed.event.has_value()) {
        return parsed;
    }
    const auto& envelope = *parsed.event;
    if (envelope.event == "started" || envelope.event == "progress" ||
        envelope.event == "artifact" || envelope.event == "completed") {
        application::WorkerEvent event{
            .kind = event_kind(envelope.event),
            .task_kind = envelope.task_kind,
            .sample_id = envelope.sample_id.empty()
                             ? std::nullopt
                             : std::optional<std::string>{envelope.sample_id},
            .progress = envelope.progress,
            .artifact_uris = envelope.artifact_uris,
            .proof_stress = envelope.proof_stress,
            .result_json = envelope.result_json,
        };
        lifecycle_.record_event(execution_, event, configured_sample_id_);
    }
    return parsed;
}

mbs::runtime::WorkerOutcome WorkerTaskBridge::finish(const int exit_code,
                                                     const bool cancellation_requested) {
    if (finished_) {
        throw std::logic_error{"worker task has already finished"};
    }
    const auto outcome = session_.finish(exit_code, cancellation_requested);
    lifecycle_.finish(execution_, terminal_status(outcome.status), outcome.error);
    finished_ = true;
    return outcome;
}

} // namespace mbs::infrastructure::runtime
