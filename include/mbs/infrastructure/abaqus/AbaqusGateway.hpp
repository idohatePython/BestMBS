#pragma once

#include "mbs/runtime/ProcessRunner.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mbs::infrastructure::abaqus {

enum class AbaqusOperation { simulation, preprocess_validation, postprocess, animation };

struct AbaqusRequest final {
    AbaqusOperation operation{AbaqusOperation::simulation};
    std::filesystem::path command{"abaqus"};
    std::filesystem::path script;
    std::filesystem::path config;
    std::filesystem::path working_directory;
    std::string job_name{"job-intlck-tpms"};
    std::filesystem::path expected_artifact;
    double target_step_time{1.0};
    bool monitor{true};
};

struct AbaqusEvent final {
    std::string kind;
    std::string message;
    std::optional<double> progress;
};

struct AbaqusResult final {
    std::filesystem::path artifact;
    int exit_code{};
};

using AbaqusEventSink = std::function<void(const AbaqusEvent&)>;

class AbaqusMonitor final {
  public:
    AbaqusMonitor(std::filesystem::path working_directory, std::string job_name,
                  double target_step_time);

    void establish_baseline();
    void poll(const AbaqusEventSink& sink);
    [[nodiscard]] std::optional<std::filesystem::path> failure_marker() const;

  private:
    std::filesystem::path working_directory_;
    std::string job_name_;
    double target_step_time_;
    std::size_t progress_lines_{};
    std::string last_status_key_;
};

class AbaqusGateway final {
  public:
    explicit AbaqusGateway(runtime::IProcessRunner& processes) noexcept;

    [[nodiscard]] AbaqusResult run(const AbaqusRequest& request,
                                   runtime::CancellationToken& cancellation,
                                   const AbaqusEventSink& event_sink);
    [[nodiscard]] int terminate(std::string_view command, std::string_view job_name,
                                const std::filesystem::path& working_directory);

  private:
    runtime::IProcessRunner& processes_;
};

[[nodiscard]] std::string_view to_string(AbaqusOperation operation) noexcept;

} // namespace mbs::infrastructure::abaqus
