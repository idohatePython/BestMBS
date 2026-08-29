#include "TestSupport.hpp"
#include "mbs/infrastructure/abaqus/AbaqusGateway.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("mbs4-abaqus-test-" + std::to_string(++sequence_))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    inline static int sequence_{};
    std::filesystem::path path_;
};

void touch(const std::filesystem::path& path, const std::string_view content = {}) {
    std::ofstream output{path, std::ios::binary};
    output << content;
}

class ProcessRunnerStub final : public mbs::runtime::IProcessRunner {
  public:
    mbs::runtime::ProcessResult run(const mbs::runtime::ProcessRequest& request,
                                    mbs::runtime::CancellationToken& cancellation,
                                    const mbs::runtime::ProcessOutputSink& sink) override {
        requests.push_back(request);
        sink("fake Abaqus output");
        if (request.on_tick) {
            request.on_tick();
        }
        if (cancel_during_run) {
            cancellation.request();
            if (request.graceful_cancel) {
                request.graceful_cancel();
            }
            return {.exit_code = 130, .cancelled = true, .forced_termination = false, .output = {}};
        }
        if (!artifact_to_create.empty()) {
            touch(artifact_to_create);
        }
        return {
            .exit_code = exit_code, .cancelled = false, .forced_termination = false, .output = {}};
    }

    std::vector<mbs::runtime::ProcessRequest> requests;
    std::filesystem::path artifact_to_create;
    int exit_code{};
    bool cancel_during_run{};
};

bool throws_runtime(const auto& callable) {
    try {
        callable();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

mbs::infrastructure::abaqus::AbaqusRequest request_for(const TemporaryDirectory& temporary) {
    const auto script = temporary.path() / "preprocess.py";
    const auto config = temporary.path() / "simulation.json";
    touch(script, "# fake");
    touch(config, "{}");
    return {
        .operation = mbs::infrastructure::abaqus::AbaqusOperation::simulation,
        .command = "fake-abaqus.exe",
        .script = script,
        .config = config,
        .working_directory = temporary.path(),
        .job_name = "job-test",
        .expected_artifact = temporary.path() / "job-test.odb",
        .target_step_time = 1.0,
        .monitor = true,
    };
}

void test_monitor(int& failures) {
    TemporaryDirectory temporary;
    touch(temporary.path() / "TPMS11_PrePrc_progress.log", "old line\n");
    touch(temporary.path() / "job-test.sta", "1 1 1 0 1 1 0.10 0.25 0.10\n");
    mbs::infrastructure::abaqus::AbaqusMonitor monitor{temporary.path(), "job-test", 1.0};
    monitor.establish_baseline();
    touch(temporary.path() / "TPMS11_PrePrc_progress.log", "old line\nJob submitted\n");
    touch(temporary.path() / "job-test.sta",
          "1 1 1 0 1 1 0.10 0.25 0.10\n1 2 1 0 1 1 0.10 0.75 0.10\n");
    std::vector<mbs::infrastructure::abaqus::AbaqusEvent> events;
    monitor.poll([&](const auto& event) { events.push_back(event); });
    MBS_CHECK(events.size() == 2);
    MBS_CHECK(events[0].message == "Job submitted");
    MBS_CHECK(events[1].progress == 0.75);
    events.clear();
    monitor.poll([&](const auto& event) { events.push_back(event); });
    MBS_CHECK(events.empty());
    touch(temporary.path() / "job-test.dat", "header\n***ERROR invalid element\n");
    MBS_CHECK(monitor.failure_marker() == temporary.path() / "job-test.dat");
}

void test_gateway_success_and_failure(int& failures) {
    TemporaryDirectory temporary;
    auto request = request_for(temporary);
    touch(temporary.path() / "TPMS11_PrePrc_progress.log", "");
    touch(temporary.path() / "job-test.sta", "1 1 1 0 1 1 0.10 0.50 0.10\n");
    ProcessRunnerStub processes;
    processes.artifact_to_create = request.expected_artifact;
    mbs::infrastructure::abaqus::AbaqusGateway gateway{processes};
    mbs::runtime::CancellationToken cancellation;
    std::vector<mbs::infrastructure::abaqus::AbaqusEvent> events;
    const auto result =
        gateway.run(request, cancellation, [&](const auto& event) { events.push_back(event); });
    MBS_CHECK(result.artifact == request.expected_artifact);
    MBS_CHECK(processes.requests.size() == 1);
    MBS_CHECK(processes.requests[0].arguments.size() == 5);
    MBS_CHECK(processes.requests[0].arguments[0] == "cae");
    MBS_CHECK(processes.requests[0].arguments[1].starts_with("noGUI="));
    MBS_CHECK(processes.requests[0].arguments[4] == "1");
    MBS_CHECK(events.front().kind == "started");
    MBS_CHECK(events.back().kind == "completed");

    auto postprocess_request = request;
    postprocess_request.operation = mbs::infrastructure::abaqus::AbaqusOperation::postprocess;
    postprocess_request.expected_artifact = temporary.path() / "proof-result.json";
    postprocess_request.monitor = false;
    processes.artifact_to_create = postprocess_request.expected_artifact;
    static_cast<void>(gateway.run(postprocess_request, cancellation, [](const auto&) {}));
    MBS_CHECK(processes.requests.back().arguments.size() == 3);
    MBS_CHECK(processes.requests.back().arguments[0] == "python");
    MBS_CHECK(std::filesystem::path{processes.requests.back().arguments[1]}.filename() ==
              request.script.filename());
    MBS_CHECK(std::filesystem::path{processes.requests.back().arguments[1]}.parent_path() !=
              request.script.parent_path());
    MBS_CHECK(std::filesystem::path{processes.requests.back().arguments[2]}.filename() ==
              "request.json");

    std::filesystem::remove(request.expected_artifact);
    processes.artifact_to_create.clear();
    MBS_CHECK(throws_runtime(
        [&] { static_cast<void>(gateway.run(request, cancellation, [](const auto&) {})); }));
}

void test_gateway_cancellation(int& failures) {
    TemporaryDirectory temporary;
    auto request = request_for(temporary);
    ProcessRunnerStub processes;
    processes.cancel_during_run = true;
    mbs::infrastructure::abaqus::AbaqusGateway gateway{processes};
    mbs::runtime::CancellationToken cancellation;
    MBS_CHECK(throws_runtime(
        [&] { static_cast<void>(gateway.run(request, cancellation, [](const auto&) {})); }));
    MBS_CHECK(processes.requests.size() == 2);
    MBS_CHECK(processes.requests[1].arguments[0] == "terminate");
    MBS_CHECK(processes.requests[1].arguments[1] == "job=job-test");
}

} // namespace

int main() {
    const auto launch_root =
        (std::filesystem::temp_directory_path() / "mbs4-abaqus-launch-tests").string();
#ifdef _WIN32
    _putenv_s("MBS_TEMP_ROOT", launch_root.c_str());
#else
    setenv("MBS_TEMP_ROOT", launch_root.c_str(), 1);
#endif
    int failures = 0;
    test_monitor(failures);
    test_gateway_success_and_failure(failures);
    test_gateway_cancellation(failures);
    return failures == 0 ? 0 : 1;
}
