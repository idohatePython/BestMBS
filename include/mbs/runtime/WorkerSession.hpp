#pragma once

#include "mbs/runtime/EventEnvelope.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mbs::runtime {

enum class WorkerLineKind { log, event, rejected_event };

struct WorkerLine final {
    WorkerLineKind kind{WorkerLineKind::log};
    std::string text;
    std::optional<EventEnvelope> event;
};

enum class WorkerTerminalStatus { succeeded, failed, cancelled };

struct WorkerOutcome final {
    WorkerTerminalStatus status{WorkerTerminalStatus::failed};
    std::string error;
};

class WorkerSession final {
  public:
    explicit WorkerSession(std::string expected_task_id);

    [[nodiscard]] WorkerLine consume(std::string_view line);
    [[nodiscard]] WorkerOutcome finish(int exit_code, bool cancellation_requested) const;
    [[nodiscard]] bool saw_terminal_event() const noexcept;

  private:
    std::string expected_task_id_;
    bool saw_completed_{};
    bool saw_failed_{};
    std::string terminal_error_;
};

} // namespace mbs::runtime
