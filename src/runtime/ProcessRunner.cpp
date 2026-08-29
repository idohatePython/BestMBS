#include "mbs/runtime/ProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace mbs::runtime {

void CancellationToken::request() noexcept { requested_.store(true); }

bool CancellationToken::requested() const noexcept { return requested_.load(); }

#ifdef _WIN32
namespace {

class Handle final {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() {
        if (valid()) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(value_, nullptr); }

  private:
    HANDLE value_{};
};

std::wstring widen(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error{"process argument is not valid UTF-8"};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    static_cast<void>(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), result.data(), size));
    return result;
}

std::wstring quote(const std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{argument};
    }
    std::wstring output{L'\"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            output.append(backslashes * 2 + 1, L'\\');
            output += L'\"';
        } else {
            output.append(backslashes, L'\\');
            output += character;
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, L'\\');
    output += L'\"';
    return output;
}

std::filesystem::path resolve_executable(const ProcessRequest& request) {
    const auto& requested = request.executable;
    if (requested.empty() || requested.is_absolute() || requested.has_parent_path()) {
        return requested;
    }
    std::vector<std::filesystem::path> directories;
    if (!request.working_directory.empty()) {
        directories.push_back(request.working_directory);
    }
    std::string search_path;
    if (const auto configured = request.environment.find("PATH");
        configured != request.environment.end()) {
        search_path = configured->second;
    } else if (const auto* inherited = std::getenv("PATH"); inherited != nullptr) {
        search_path = inherited;
    }
    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const auto end = search_path.find(';', begin);
        auto entry = search_path.substr(begin, end == std::string::npos
                                                  ? search_path.size() - begin
                                                  : end - begin);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty()) {
            directories.emplace_back(entry);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    constexpr std::array<std::string_view, 5> suffixes{"", ".com", ".exe", ".bat",
                                                        ".cmd"};
    const auto suffix_count = requested.has_extension() ? 1U : suffixes.size();
    for (const auto& directory : directories) {
        for (std::size_t suffix_index = 0; suffix_index < suffix_count; ++suffix_index) {
            const auto suffix = suffixes[suffix_index];
            auto candidate = directory / requested;
            if (!suffix.empty()) {
                candidate += suffix;
            }
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return std::filesystem::absolute(candidate, error);
            }
        }
    }
    return requested;
}

std::wstring command_line(const ProcessRequest& request,
                          const std::filesystem::path& executable) {
    std::wstring child = quote(executable.wstring());
    for (const auto& argument : request.arguments) {
        child += L' ';
        child += quote(widen(argument));
    }
    auto extension = executable.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension == L".bat" || extension == L".cmd") {
        return L"cmd.exe /D /S /C \"" + child + L"\"";
    }
    return child;
}

std::vector<wchar_t> environment_block(
    const std::map<std::string, std::string, std::less<>>& overrides) {
    if (overrides.empty()) {
        return {};
    }
    std::vector<std::pair<std::wstring, std::wstring>> replacements;
    replacements.reserve(overrides.size());
    for (const auto& [key, value] : overrides) {
        replacements.emplace_back(widen(key), widen(value));
    }
    const auto folded = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    };
    std::vector<std::wstring> entries;
    LPWCH inherited = GetEnvironmentStringsW();
    if (inherited == nullptr) {
        throw std::runtime_error{"GetEnvironmentStringsW failed"};
    }
    for (const wchar_t* cursor = inherited; *cursor != L'\0';) {
        std::wstring entry{cursor};
        cursor += entry.size() + 1;
        const auto delimiter = entry.find(L'=', entry.starts_with(L'=') ? 1U : 0U);
        const auto key = delimiter == std::wstring::npos ? entry : entry.substr(0, delimiter);
        const auto overridden = std::any_of(replacements.begin(), replacements.end(),
                                            [&](const auto& replacement) {
                                                return folded(replacement.first) == folded(key);
                                            });
        if (!overridden) {
            entries.push_back(std::move(entry));
        }
    }
    FreeEnvironmentStringsW(inherited);
    for (auto& [key, value] : replacements) {
        entries.push_back(std::move(key) + L'=' + std::move(value));
    }
    std::sort(entries.begin(), entries.end(), [&](const auto& left, const auto& right) {
        return folded(left) < folded(right);
    });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::string windows_error(const std::string_view action) {
    return std::string{action} + " failed with Windows error " + std::to_string(GetLastError());
}

std::string decode_line(const std::string_view line) {
    if (line.empty()) {
        return {};
    }
    if (line.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::string{line};
    }
    const auto input_size = static_cast<int>(line.size());
    UINT code_page = CP_UTF8;
    int wide_size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), input_size, nullptr, 0);
    if (wide_size == 0) {
        code_page = 936;
        wide_size = MultiByteToWideChar(code_page, 0, line.data(), input_size, nullptr, 0);
    }
    if (wide_size == 0) {
        return std::string{line};
    }
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    static_cast<void>(MultiByteToWideChar(code_page,
                                          code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                                          line.data(), input_size, wide.data(), wide_size));
    const int utf8_size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
    if (utf8_size == 0) {
        return std::string{line};
    }
    std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, utf8.data(),
                                          utf8_size, nullptr, nullptr));
    return utf8;
}

void emit_available(HANDLE pipe, std::string& pending, ProcessResult& result,
                    const ProcessOutputSink& sink) {
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == 0 || available == 0) {
            break;
        }
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
        if (ReadFile(pipe, buffer.data(), wanted, &read, nullptr) == 0 || read == 0) {
            break;
        }
        const std::string_view chunk{buffer.data(), static_cast<std::size_t>(read)};
        result.output.append(chunk);
        pending.append(chunk);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, newline);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            sink(decode_line(line));
            pending.erase(0, newline + 1);
        }
    }
}

} // namespace
#endif

ProcessResult SystemProcessRunner::run(const ProcessRequest& request,
                                       CancellationToken& cancellation,
                                       const ProcessOutputSink& output_sink) {
    if (request.executable.empty()) {
        throw std::invalid_argument{"process executable is required"};
    }
#ifndef _WIN32
    static_cast<void>(cancellation);
    static_cast<void>(output_sink);
    throw std::runtime_error{"SystemProcessRunner is currently implemented for Windows"};
#else
    SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                 .lpSecurityDescriptor = nullptr,
                                 .bInheritHandle = TRUE};
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (CreatePipe(&read_raw, &write_raw, &security, 0) == 0) {
        throw std::runtime_error{windows_error("CreatePipe")};
    }
    Handle read_pipe{read_raw};
    Handle write_pipe{write_raw};
    if (SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
        throw std::runtime_error{windows_error("SetHandleInformation")};
    }

    SIZE_T attribute_size = 0;
    static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size));
    if (attribute_size == 0) {
        throw std::runtime_error{windows_error("InitializeProcThreadAttributeList size query")};
    }
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) == 0) {
        throw std::runtime_error{windows_error("InitializeProcThreadAttributeList")};
    }
    std::array inherited_handles{write_pipe.get()};
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited_handles.data(), sizeof(inherited_handles), nullptr,
                                  nullptr) == 0) {
        DeleteProcThreadAttributeList(attributes);
        throw std::runtime_error{windows_error("UpdateProcThreadAttribute")};
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_pipe.get();
    startup.StartupInfo.hStdError = write_pipe.get();
    startup.StartupInfo.hStdInput = nullptr;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info{};
    const auto executable = resolve_executable(request);
    auto command = command_line(request, executable);
    const auto working_directory =
        request.working_directory.empty() ? std::wstring{} : request.working_directory.wstring();
    auto child_environment = environment_block(request.environment);
    const BOOL created =
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                           (child_environment.empty() ? 0U : CREATE_UNICODE_ENVIRONMENT),
                       child_environment.empty() ? nullptr : child_environment.data(),
                       working_directory.empty() ? nullptr : working_directory.c_str(),
                       &startup.StartupInfo, &process_info);
    DeleteProcThreadAttributeList(attributes);
    if (created == 0) {
        throw std::runtime_error{windows_error("CreateProcessW")};
    }
    Handle process{process_info.hProcess};
    Handle thread{process_info.hThread};
    write_pipe = Handle{};

    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (job.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits)) == 0 ||
            AssignProcessToJobObject(job.get(), process.get()) == 0) {
            job = Handle{};
        }
    }

    ProcessResult result;
    std::string pending;
    bool graceful_requested = false;
    bool forced = false;
    auto cancellation_time = std::chrono::steady_clock::time_point{};
    while (WaitForSingleObject(process.get(), 20) == WAIT_TIMEOUT) {
        emit_available(read_pipe.get(), pending, result, output_sink);
        if (request.on_tick) {
            request.on_tick();
        }
        if (cancellation.requested() && !graceful_requested) {
            graceful_requested = true;
            cancellation_time = std::chrono::steady_clock::now();
            if (request.graceful_cancel) {
                request.graceful_cancel();
            }
        }
        if (graceful_requested &&
            std::chrono::steady_clock::now() - cancellation_time >= request.cancellation_grace) {
            forced = true;
            if (job.valid()) {
                static_cast<void>(TerminateJobObject(job.get(), 130));
            } else {
                static_cast<void>(TerminateProcess(process.get(), 130));
            }
            static_cast<void>(WaitForSingleObject(process.get(), INFINITE));
            break;
        }
    }
    emit_available(read_pipe.get(), pending, result, output_sink);
    if (!pending.empty()) {
        output_sink(decode_line(pending));
    }
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process.get(), &exit_code) == 0) {
        throw std::runtime_error{windows_error("GetExitCodeProcess")};
    }
    result.exit_code = static_cast<int>(exit_code);
    result.cancelled = graceful_requested;
    result.forced_termination = forced;
    return result;
#endif
}

} // namespace mbs::runtime
