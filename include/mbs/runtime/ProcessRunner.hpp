#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::runtime {

class CancellationToken final {
  public:
    void request() noexcept;
    [[nodiscard]] bool requested() const noexcept;

  private:
    std::atomic_bool requested_{};
};

struct ProcessRequest final {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::map<std::string, std::string, std::less<>> environment;
    std::chrono::milliseconds cancellation_grace{3'000};
    std::function<void()> graceful_cancel;
    std::function<void()> on_tick;
};

struct ProcessResult final {
    int exit_code{-1};
    bool cancelled{};
    bool forced_termination{};
    std::string output;
};

using ProcessOutputSink = std::function<void(std::string_view)>;

class IProcessRunner {
  public:
    virtual ~IProcessRunner() = default;
    [[nodiscard]] virtual ProcessResult run(const ProcessRequest& request,
                                            CancellationToken& cancellation,
                                            const ProcessOutputSink& output_sink) = 0;
};

class SystemProcessRunner final : public IProcessRunner {
  public:
    [[nodiscard]] ProcessResult run(const ProcessRequest& request, CancellationToken& cancellation,
                                    const ProcessOutputSink& output_sink) override;
};

} // namespace mbs::runtime
