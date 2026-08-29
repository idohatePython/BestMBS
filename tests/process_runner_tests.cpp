#include "TestSupport.hpp"
#include "mbs/runtime/EventEnvelope.hpp"
#include "mbs/runtime/ProcessRunner.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>

int main(const int argc, char** argv) {
    int failures = 0;
#ifdef _WIN32
    mbs::runtime::SystemProcessRunner processes;
    mbs::runtime::CancellationToken cancellation;
    std::vector<std::string> output;
    const auto process_result = processes.run(
        {.executable = "cmd.exe",
         .arguments = {"/D", "/C", "echo process-ready"},
         .working_directory = {},
         .environment = {},
         .cancellation_grace = std::chrono::seconds{3},
         .graceful_cancel = {},
         .on_tick = {}},
        cancellation, [&](const std::string_view line) { output.emplace_back(line); });
    MBS_CHECK(process_result.exit_code == 0);
    MBS_CHECK(!output.empty());
    MBS_CHECK(output[0] == "process-ready");

    output.clear();
    const auto environment_result = processes.run(
        {.executable = "cmd.exe",
         .arguments = {"/D", "/C", "echo %MBS_PROCESS_ENV_TEST%"},
         .working_directory = {},
         .environment = {{"MBS_PROCESS_ENV_TEST", "override-ready"}},
         .cancellation_grace = std::chrono::seconds{3},
         .graceful_cancel = {},
         .on_tick = {}},
        cancellation, [&](const std::string_view line) { output.emplace_back(line); });
    MBS_CHECK(environment_result.exit_code == 0);
    MBS_CHECK(!output.empty());
    MBS_CHECK(output[0] == "override-ready");

    const auto batch = std::filesystem::temp_directory_path() / "mbs4-process-runner-test.cmd";
    {
        std::ofstream script{batch, std::ios::binary};
        script << "@echo batch-ready\r\n";
    }
    output.clear();
    const auto batch_result = processes.run(
        {.executable = batch,
         .arguments = {},
         .working_directory = batch.parent_path(),
         .environment = {},
         .cancellation_grace = std::chrono::seconds{3},
         .graceful_cancel = {},
         .on_tick = {}},
        cancellation, [&](const std::string_view line) { output.emplace_back(line); });
    std::filesystem::remove(batch);
    MBS_CHECK(batch_result.exit_code == 0);
    MBS_CHECK(!output.empty());
    MBS_CHECK(output[0] == "batch-ready");

    output.clear();
    const auto bare_batch = batch.parent_path() / "mbs4-bare-command.cmd";
    {
        std::ofstream script{bare_batch, std::ios::binary};
        script << "@echo bare-command-ready\r\n";
    }
    const auto bare_result = processes.run(
        {.executable = "mbs4-bare-command",
         .arguments = {},
         .working_directory = batch.parent_path(),
         .environment = {{"PATH", batch.parent_path().generic_string()}},
         .cancellation_grace = std::chrono::seconds{3},
         .graceful_cancel = {},
         .on_tick = {}},
        cancellation, [&](const std::string_view line) { output.emplace_back(line); });
    std::filesystem::remove(bare_batch);
    MBS_CHECK(bare_result.exit_code == 0);
    MBS_CHECK(!output.empty());
    MBS_CHECK(output[0] == "bare-command-ready");

    mbs::runtime::CancellationToken stop;
    stop.request();
    bool graceful_called = false;
    const auto cancelled = processes.run({.executable = "cmd.exe",
                                          .arguments = {"/D", "/C", "ping 127.0.0.1 -n 6 > nul"},
                                          .working_directory = {},
                                          .environment = {},
                                          .cancellation_grace = std::chrono::milliseconds{50},
                                          .graceful_cancel = [&] { graceful_called = true; },
                                          .on_tick = {}},
                                         stop, [](std::string_view) {});
    MBS_CHECK(cancelled.cancelled);
    MBS_CHECK(cancelled.forced_termination);
    MBS_CHECK(graceful_called);

    MBS_CHECK(argc == 2);
    if (argc == 2) {
        const auto worker_directory =
            std::filesystem::temp_directory_path() / "mbs4-worker-e2e-test";
        std::error_code cleanup_error;
        std::filesystem::remove_all(worker_directory, cleanup_error);
        std::filesystem::create_directories(worker_directory);
        const auto launch_root = (worker_directory / "launch").string();
        _putenv_s("MBS_TEMP_ROOT", launch_root.c_str());
        const auto fake_abaqus = worker_directory / "fake-abaqus.cmd";
        const auto abaqus_script = worker_directory / "preprocess.py";
        const auto config = worker_directory / "simulation.json";
        {
            std::ofstream file{fake_abaqus, std::ios::binary};
            file << "@echo fake Abaqus solver\r\n@type nul > \""
                 << (worker_directory / "job-test.odb").string()
                 << "\"\r\n@exit /b 0\r\n";
        }
        {
            std::ofstream file{abaqus_script, std::ios::binary};
            file << "# fake Abaqus script\n";
        }
        {
            std::ofstream file{config, std::ios::binary};
            file << "{}\n";
        }
        output.clear();
        mbs::runtime::CancellationToken worker_cancellation;
        const auto worker_result = processes.run(
            {.executable = argv[1],
             .arguments = {"abaqus", "--operation", "simulation", "--task-id", "task-e2e",
                           "--run-id", "run-e2e", "--script", abaqus_script.generic_string(),
                           "--config", config.generic_string(), "--work-dir",
                           worker_directory.generic_string(), "--command",
                           fake_abaqus.generic_string(), "--job-name", "job-test"},
             .working_directory = worker_directory,
             .environment = {},
             .cancellation_grace = std::chrono::seconds{3},
             .graceful_cancel = {},
             .on_tick = {}},
            worker_cancellation, [&](const std::string_view line) { output.emplace_back(line); });
        MBS_CHECK(worker_result.exit_code == 0);
        MBS_CHECK(std::filesystem::is_regular_file(worker_directory / "job-test.odb"));
        bool completed_event = false;
        for (const auto& line : output) {
            const auto event = mbs::runtime::EventEnvelope::decode(line);
            if (event.has_value() && event->event == "completed" && event->task_id == "task-e2e" &&
                event->artifact_uris.find("odb_path") != event->artifact_uris.end()) {
                completed_event = true;
            }
        }
        MBS_CHECK(completed_event);

        const auto result_path = worker_directory / "proof-result.json";
        {
            std::ofstream file{result_path, std::ios::binary};
            file << "{\"status\":\"ok\",\"proof_stress\":12.75,\"strain\":[0,0.01],"
                    "\"stress\":[0,12.75],\"offset_stress\":[-2,10]}\n";
        }
        output.clear();
        const auto postprocess_result = processes.run(
            {.executable = argv[1],
             .arguments = {"abaqus", "--operation", "postprocess", "--task-id", "post-e2e",
                           "--run-id", "run-e2e", "--sample-id", "mbs-1", "--script",
                           abaqus_script.generic_string(), "--config", config.generic_string(),
                           "--work-dir", worker_directory.generic_string(), "--command",
                           fake_abaqus.generic_string(), "--job-name", "job-test", "--expected",
                           result_path.generic_string()},
             .working_directory = worker_directory,
             .environment = {},
             .cancellation_grace = std::chrono::seconds{3},
             .graceful_cancel = {},
             .on_tick = {}},
            worker_cancellation, [&](const std::string_view line) { output.emplace_back(line); });
        MBS_CHECK(postprocess_result.exit_code == 0);
        bool postprocess_completed = false;
        for (const auto& line : output) {
            const auto event = mbs::runtime::EventEnvelope::decode(line);
            if (event.has_value() && event->event == "completed" &&
                event->task_id == "post-e2e" && event->proof_stress == 12.75 &&
                event->artifact_uris.at("result_path") == result_path.generic_string() &&
                event->result_json.find("\"stress\"") != std::string::npos) {
                postprocess_completed = true;
            }
        }
        MBS_CHECK(postprocess_completed);
        std::filesystem::remove_all(worker_directory, cleanup_error);
    }
#endif
    return failures == 0 ? 0 : 1;
}
