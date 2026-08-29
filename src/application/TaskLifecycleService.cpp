#include "mbs/application/TaskLifecycleService.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mbs::application {
namespace {

std::optional<std::string> effective_sample_id(const WorkerEvent& event,
                                               std::optional<std::string> configured) {
    if (event.sample_id.has_value() && !event.sample_id->empty()) {
        return event.sample_id;
    }
    if (configured.has_value() && configured->empty()) {
        return std::nullopt;
    }
    return configured;
}

} // namespace

TaskLifecycleService::TaskLifecycleService(ITaskLifecycleStore& store, IIdGenerator& ids) noexcept
    : store_(store), ids_(ids) {}

TaskExecution TaskLifecycleService::start(std::string kind, std::string request_json,
                                          std::optional<std::string> sample_id) {
    if (kind.empty() || request_json.empty()) {
        throw std::invalid_argument{"task kind and request JSON are required"};
    }
    const auto task_id = ids_.generate("task");
    const auto run_id = ids_.generate("run");
    if (task_id.empty() || run_id.empty()) {
        throw std::runtime_error{"id generator returned an empty identifier"};
    }
    const domain::Task task{
        .id = task_id,
        .kind = kind,
        .status = domain::TaskStatus::running,
        .sample_id = sample_id,
        .run_id = run_id,
        .progress = 0.0,
        .error = {},
    };
    const domain::Run run{
        .id = run_id,
        .kind = std::move(kind),
        .status = domain::TaskStatus::running,
        .sample_id = std::move(sample_id),
        .request_json = std::move(request_json),
        .error = {},
    };
    store_.start(task, run);
    return {.task_id = task_id, .run_id = run_id};
}

void TaskLifecycleService::record_event(const TaskExecution& execution, const WorkerEvent& event,
                                        std::optional<std::string> configured_sample_id) {
    if (execution.task_id.empty() || execution.run_id.empty()) {
        throw std::invalid_argument{"task execution identifiers are required"};
    }
    const auto sample_id = effective_sample_id(event, std::move(configured_sample_id));
    std::vector<ArtifactDraft> artifacts;
    if (event.kind == WorkerEventKind::artifact || event.kind == WorkerEventKind::completed) {
        static constexpr std::array mappings{
            std::pair{"path", "artifact"},
            std::pair{"output_dir", "design_output"},
            std::pair{"odb_path", "abaqus_odb"},
            std::pair{"animation_manifest", "animation_manifest"},
            std::pair{"result_path", "postprocess_result"},
            std::pair{"report_path", "contact_risk_report"},
        };
        for (const auto& [field, artifact_kind] : mappings) {
            const auto iterator = event.artifact_uris.find(field);
            if (iterator != event.artifact_uris.end() && !iterator->second.empty()) {
                artifacts.push_back({.kind = artifact_kind,
                                     .uri = iterator->second,
                                     .sample_id = sample_id,
                                     .run_id = execution.run_id});
            }
        }
    }

    std::vector<MetricDraft> metrics;
    if (event.kind == WorkerEventKind::completed && event.task_kind == "postprocess" &&
        event.proof_stress.has_value()) {
        if (!std::isfinite(*event.proof_stress)) {
            throw std::invalid_argument{"proof stress must be finite"};
        }
        metrics.push_back({.name = "proof_stress",
                           .value = *event.proof_stress,
                           .unit = "MPa",
                           .sample_id = sample_id,
                           .run_id = execution.run_id,
                           .details_json = event.result_json});
    }

    auto progress = event.progress;
    if (progress.has_value()) {
        if (!std::isfinite(*progress)) {
            throw std::invalid_argument{"task progress must be finite"};
        }
        if (*progress > 1.0) {
            progress = *progress / 100.0;
        }
        if (*progress < 0.0 || *progress > 1.0) {
            throw std::invalid_argument{"task progress must be in [0, 1] or [0, 100]"};
        }
    }
    store_.record_event(execution.task_id, execution.run_id, sample_id, artifacts, metrics,
                        progress);
}

void TaskLifecycleService::finish(const TaskExecution& execution, const domain::TaskStatus status,
                                  const std::string_view error) {
    if (!domain::is_terminal(status)) {
        throw std::invalid_argument{"finish requires a terminal task status"};
    }
    store_.finish(execution.task_id, execution.run_id, status, error);
}

int TaskLifecycleService::recover_interrupted() { return store_.recover_interrupted(); }

} // namespace mbs::application
