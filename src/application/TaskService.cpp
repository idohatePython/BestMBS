#include "mbs/application/TaskService.hpp"

#include "mbs/domain/Validation.hpp"

#include <cmath>
#include <utility>

namespace mbs::application {

TaskService::TaskService(ITaskRepository& repository) noexcept : repository_(repository) {}

void TaskService::create(const domain::Task& task) {
    domain::require_valid(task.validation_errors());
    if (task.status != domain::TaskStatus::queued || std::abs(task.progress) > 1.0e-12) {
        throw domain::ValidationError{{"a new task must be queued with zero progress"}};
    }
    repository_.create(task);
}

bool TaskService::transition(const std::string_view task_id, const domain::TaskStatus status,
                             const std::optional<double> progress, std::string error) {
    auto task = repository_.find(task_id);
    if (!task.has_value() || !domain::transition_allowed(task->status, status)) {
        return false;
    }
    if (progress.has_value()) {
        if (!std::isfinite(*progress) || *progress < 0.0 || *progress > 1.0) {
            return false;
        }
        task->progress = *progress;
    }
    if (status == domain::TaskStatus::succeeded) {
        task->progress = 1.0;
        error.clear();
    }
    task->status = status;
    task->error = std::move(error);
    domain::require_valid(task->validation_errors());
    repository_.update(*task);
    return true;
}

std::optional<domain::Task> TaskService::find(const std::string_view task_id) const {
    return repository_.find(task_id);
}

int TaskService::recover_interrupted() { return repository_.interrupt_running(); }

} // namespace mbs::application
