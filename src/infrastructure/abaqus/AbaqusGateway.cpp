#include "mbs/infrastructure/abaqus/AbaqusGateway.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mbs::infrastructure::abaqus {
namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<double> status_progress(const std::string& line, const double target_step_time,
                                      std::string& key) {
    std::istringstream input{line};
    std::vector<std::string> parts;
    for (std::string part; input >> part;) {
        parts.push_back(std::move(part));
    }
    if (parts.size() < 8 || parts[0].find_first_not_of("0123456789") != std::string::npos ||
        parts[1].find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    key = parts[0] + ':' + parts[1] + ':' + parts[2];
    try {
        const double step_time = std::stod(parts[7]);
        return std::clamp(step_time / target_step_time, 0.0, 1.0);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void validate(const AbaqusRequest& request) {
    if (request.command.empty() || request.script.empty() || request.config.empty() ||
        request.working_directory.empty() || request.job_name.empty()) {
        throw std::invalid_argument{
            "Abaqus command, script, config, work directory and job name are required"};
    }
    if (!std::filesystem::is_regular_file(request.script)) {
        throw std::invalid_argument{"Abaqus script does not exist: " + request.script.string()};
    }
    if (!std::filesystem::is_regular_file(request.config)) {
        throw std::invalid_argument{"Abaqus config does not exist: " + request.config.string()};
    }
    if (!std::isfinite(request.target_step_time) || request.target_step_time <= 0.0) {
        throw std::invalid_argument{"Abaqus target step time must be positive"};
    }
}

std::map<std::string, std::string, std::less<>>
abaqus_environment(const AbaqusRequest& request,
                   const std::filesystem::path& launch_directory) {
    std::string clean_path;
    const auto append = [&](const std::string_view entry) {
        if (!entry.empty()) {
            if (!clean_path.empty()) {
                clean_path.push_back(';');
            }
            clean_path.append(entry);
        }
    };
    append(request.command.parent_path().generic_string());
    if (const auto* inherited = std::getenv("PATH"); inherited != nullptr) {
        std::string_view path{inherited};
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const auto end = path.find(';', begin);
            const auto entry = path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                                               : end - begin);
            auto lower = std::string{entry};
            std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const bool conflicting =
                lower.find("mingw") != std::string::npos ||
                lower.find("\\qt\\") != std::string::npos ||
                lower.find("/qt/") != std::string::npos ||
                lower.find("anaconda") != std::string::npos ||
                lower.find("conda") != std::string::npos ||
                lower.find("codex-runtimes") != std::string::npos ||
                lower.find("dependencies\\bin\\override") != std::string::npos;
            if (!conflicting && entry != request.command.parent_path().generic_string()) {
                append(entry);
            }
            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    const auto temporary = launch_directory / "temp";
    std::filesystem::create_directories(temporary);
    return {{"PATH", std::move(clean_path)},
            {"TEMP", temporary.generic_string()},
            {"TMP", temporary.generic_string()},
            {"PYTHONHOME", ""},
            {"PYTHONPATH", ""},
            {"QT_PLUGIN_PATH", ""},
            {"QML2_IMPORT_PATH", ""}};
}

std::filesystem::path make_launch_directory(const std::string_view job_name) {
    const auto* configured = std::getenv("MBS_TEMP_ROOT");
    const auto root = configured != nullptr && *configured != '\0'
                          ? std::filesystem::path{configured}
                          : std::filesystem::path{"C:/temp/TPMS11"};
    std::string safe_name;
    safe_name.reserve(job_name.size());
    for (const unsigned char character : job_name) {
        safe_name.push_back(std::isalnum(character) || character == '-' || character == '_'
                                ? static_cast<char>(character)
                                : '_');
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = root / "_abaqus_launch" /
                           (safe_name + '-' + std::to_string(stamp));
    std::filesystem::create_directories(directory / "temp");
    return directory;
}

class LaunchDirectory final {
  public:
    explicit LaunchDirectory(const std::string_view job_name)
        : path_(make_launch_directory(job_name)) {}
    ~LaunchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    LaunchDirectory(const LaunchDirectory&) = delete;
    LaunchDirectory& operator=(const LaunchDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace

AbaqusMonitor::AbaqusMonitor(std::filesystem::path working_directory, std::string job_name,
                             const double target_step_time)
    : working_directory_(std::move(working_directory)), job_name_(std::move(job_name)),
      target_step_time_(target_step_time) {
    if (working_directory_.empty() || job_name_.empty() || !std::isfinite(target_step_time_) ||
        target_step_time_ <= 0.0) {
        throw std::invalid_argument{"invalid Abaqus monitor configuration"};
    }
}

void AbaqusMonitor::establish_baseline() {
    progress_lines_ = read_lines(working_directory_ / "TPMS11_PrePrc_progress.log").size();
    for (const auto& line : read_lines(working_directory_ / (job_name_ + ".sta"))) {
        std::string key;
        if (status_progress(line, target_step_time_, key).has_value()) {
            last_status_key_ = std::move(key);
        }
    }
}

void AbaqusMonitor::poll(const AbaqusEventSink& sink) {
    const auto progress = read_lines(working_directory_ / "TPMS11_PrePrc_progress.log");
    if (progress.size() < progress_lines_) {
        progress_lines_ = 0;
    }
    for (std::size_t index = progress_lines_; index < progress.size(); ++index) {
        sink({.kind = "log", .message = progress[index], .progress = std::nullopt});
    }
    progress_lines_ = progress.size();

    std::optional<double> latest;
    std::string latest_key;
    for (const auto& line : read_lines(working_directory_ / (job_name_ + ".sta"))) {
        std::string key;
        if (const auto value = status_progress(line, target_step_time_, key); value.has_value()) {
            latest = value;
            latest_key = std::move(key);
        }
    }
    if (latest.has_value() && latest_key != last_status_key_) {
        last_status_key_ = std::move(latest_key);
        sink({.kind = "progress", .message = "Abaqus analysis progress", .progress = latest});
    }
}

std::optional<std::filesystem::path> AbaqusMonitor::failure_marker() const {
    static constexpr std::string_view patterns[]{
        "Abaqus/Analysis exited with errors", "Analysis Input File Processor exited with an error",
        "THE PROGRAM HAS DISCOVERED", "***ERROR"};
    for (const auto* suffix : {"log", "dat", "msg"}) {
        const auto path = working_directory_ / (job_name_ + '.' + suffix);
        std::ifstream input{path, std::ios::binary};
        if (!input) {
            continue;
        }
        const std::string content{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
        for (const auto pattern : patterns) {
            if (content.find(pattern) != std::string::npos) {
                return path;
            }
        }
    }
    return std::nullopt;
}

AbaqusGateway::AbaqusGateway(runtime::IProcessRunner& processes) noexcept : processes_(processes) {}

AbaqusResult AbaqusGateway::run(const AbaqusRequest& request,
                                runtime::CancellationToken& cancellation,
                                const AbaqusEventSink& event_sink) {
    validate(request);
    std::filesystem::create_directories(request.working_directory);
    const LaunchDirectory launch{request.job_name};
    const auto python_root = request.script.parent_path().parent_path().parent_path().parent_path()
                                 .parent_path();
    const auto package_root = python_root / "mbs";
    const auto staged_python_root = launch.path() / "python";
    std::filesystem::path staged_script;
    if (std::filesystem::is_directory(package_root)) {
        std::filesystem::create_directories(staged_python_root);
        std::filesystem::copy(package_root, staged_python_root / "mbs",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
        const auto relative_script = std::filesystem::relative(request.script, python_root);
        staged_script = staged_python_root / relative_script;
    } else {
        staged_script = launch.path() / request.script.filename();
        std::filesystem::copy_file(request.script, staged_script,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    const auto staged_config = launch.path() / "request.json";
    std::filesystem::copy_file(request.config, staged_config,
                               std::filesystem::copy_options::overwrite_existing);
    AbaqusMonitor monitor{request.working_directory, request.job_name, request.target_step_time};
    monitor.establish_baseline();
    auto arguments = request.operation == AbaqusOperation::postprocess
                         ? std::vector<std::string>{"python", staged_script.generic_string(),
                                                    staged_config.generic_string()}
                         : std::vector<std::string>{"cae",
                                                    "noGUI=" + staged_script.generic_string(),
                                                    "--", staged_config.generic_string()};
    if (request.operation == AbaqusOperation::simulation && request.monitor) {
        arguments.emplace_back("1");
    }
    runtime::ProcessRequest process{
        .executable = request.command,
        .arguments = arguments,
        .working_directory = launch.path(),
        .environment = abaqus_environment(request, launch.path()),
        .cancellation_grace = std::chrono::seconds{5},
        .graceful_cancel =
            [&] {
                static_cast<void>(terminate(request.command.generic_string(), request.job_name,
                                            request.working_directory));
            },
        .on_tick = request.monitor ? std::function<void()>{[&] { monitor.poll(event_sink); }}
                                   : std::function<void()>{},
    };
    event_sink({.kind = "started",
                .message = "Starting Abaqus " + std::string{to_string(request.operation)},
                .progress = 0.0});
    const auto result = processes_.run(process, cancellation, [&](const std::string_view line) {
        if (!line.empty()) {
            event_sink({.kind = "log", .message = std::string{line}, .progress = std::nullopt});
        }
    });
    if (request.monitor) {
        monitor.poll(event_sink);
    }
    if (result.cancelled) {
        throw std::runtime_error{"Abaqus operation was cancelled"};
    }
    if (result.exit_code != 0) {
        throw std::runtime_error{"Abaqus exited with code " + std::to_string(result.exit_code)};
    }
    if (const auto marker = monitor.failure_marker(); request.monitor && marker.has_value()) {
        throw std::runtime_error{"Abaqus reported an analysis failure in " + marker->string()};
    }
    if (!request.expected_artifact.empty() &&
        !std::filesystem::is_regular_file(request.expected_artifact)) {
        throw std::runtime_error{"Abaqus completed without expected artifact: " +
                                 request.expected_artifact.string()};
    }
    event_sink({.kind = "completed", .message = "Abaqus operation completed", .progress = 1.0});
    return {.artifact = request.expected_artifact, .exit_code = result.exit_code};
}

int AbaqusGateway::terminate(const std::string_view command, const std::string_view job_name,
                             const std::filesystem::path& working_directory) {
    if (command.empty() || job_name.empty() || working_directory.empty()) {
        throw std::invalid_argument{"Abaqus termination parameters are required"};
    }
    const LaunchDirectory launch{job_name};
    runtime::CancellationToken no_cancellation;
    const runtime::ProcessRequest request{
        .executable = std::filesystem::path{command},
        .arguments = {"terminate", "job=" + std::string{job_name}},
        .working_directory = launch.path(),
        .environment = abaqus_environment(
            {.operation = AbaqusOperation::simulation,
             .command = std::filesystem::path{command},
             .script = {},
             .config = {},
             .working_directory = working_directory,
             .job_name = std::string{job_name},
             .expected_artifact = {},
             .target_step_time = 1.0,
             .monitor = false},
            launch.path()),
        .cancellation_grace = std::chrono::seconds{3},
        .graceful_cancel = {},
        .on_tick = {},
    };
    return processes_.run(request, no_cancellation, [](std::string_view) {}).exit_code;
}

std::string_view to_string(const AbaqusOperation operation) noexcept {
    switch (operation) {
    case AbaqusOperation::simulation:
        return "simulation";
    case AbaqusOperation::preprocess_validation:
        return "preprocess-validation";
    case AbaqusOperation::postprocess:
        return "postprocess";
    case AbaqusOperation::animation:
        return "animation";
    }
    return "unknown";
}

} // namespace mbs::infrastructure::abaqus
