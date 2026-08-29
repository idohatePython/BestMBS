#pragma once

#include "mbs/application/Ports.hpp"

namespace mbs::application {

class TaskService final {
  public:
    explicit TaskService(ITaskRepository& repository) noexcept;

    void create(const domain::Task& task);
    [[nodiscard]] bool transition(std::string_view task_id, domain::TaskStatus status,
                                  std::optional<double> progress = std::nullopt,
                                  std::string error = {});
    [[nodiscard]] std::optional<domain::Task> find(std::string_view task_id) const;
    int recover_interrupted();

  private:
    ITaskRepository& repository_;
};

} // namespace mbs::application
