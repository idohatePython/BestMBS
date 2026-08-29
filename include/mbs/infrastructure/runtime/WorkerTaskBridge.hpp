#pragma once

#include "mbs/application/TaskLifecycleService.hpp"
#include "mbs/runtime/WorkerSession.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mbs::infrastructure::runtime {

class WorkerTaskBridge final {
  public:
    WorkerTaskBridge(application::TaskLifecycleService& lifecycle,
                     application::TaskExecution execution,
                     std::optional<std::string> configured_sample_id = std::nullopt);

    [[nodiscard]] mbs::runtime::WorkerLine consume(std::string_view line);
    [[nodiscard]] mbs::runtime::WorkerOutcome finish(int exit_code, bool cancellation_requested);

  private:
    application::TaskLifecycleService& lifecycle_;
    application::TaskExecution execution_;
    std::optional<std::string> configured_sample_id_;
    mbs::runtime::WorkerSession session_;
    bool finished_{};
};

} // namespace mbs::infrastructure::runtime
