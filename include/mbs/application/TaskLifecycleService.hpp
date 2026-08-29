#pragma once

#include "mbs/application/Ports.hpp"

#include <map>

namespace mbs::application {

enum class WorkerEventKind { progress, artifact, completed };

struct WorkerEvent final {
    WorkerEventKind kind{WorkerEventKind::progress};
    std::string task_kind;
    std::optional<std::string> sample_id;
    std::optional<double> progress;
    std::map<std::string, std::string, std::less<>> artifact_uris;
    std::optional<double> proof_stress;
    std::string result_json;
};

struct TaskExecution final {
    std::string task_id;
    std::string run_id;
};

class TaskLifecycleService final {
  public:
    TaskLifecycleService(ITaskLifecycleStore& store, IIdGenerator& ids) noexcept;

    [[nodiscard]] TaskExecution start(std::string kind, std::string request_json,
                                      std::optional<std::string> sample_id = std::nullopt);
    void record_event(const TaskExecution& execution, const WorkerEvent& event,
                      std::optional<std::string> configured_sample_id = std::nullopt);
    void finish(const TaskExecution& execution, domain::TaskStatus status,
                std::string_view error = {});
    int recover_interrupted();

  private:
    ITaskLifecycleStore& store_;
    IIdGenerator& ids_;
};

} // namespace mbs::application
