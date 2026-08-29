#include "mbs/runtime/EventEnvelope.hpp"

#include <charconv>
#include <cmath>
#include <sstream>

namespace mbs::runtime {
namespace {

std::optional<std::size_t> json_value_position(const std::string_view json,
                                               const std::string_view key) {
    const std::string marker = "\"" + std::string{key} + '"';
    std::size_t search_from = 0;
    while (true) {
        const auto position = json.find(marker, search_from);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        std::size_t cursor = position + marker.size();
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                        json[cursor] == '\r' || json[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor < json.size() && json[cursor] == ':') {
            ++cursor;
            while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                            json[cursor] == '\r' || json[cursor] == '\n')) {
                ++cursor;
            }
            return cursor;
        }
        search_from = position + marker.size();
    }
}

std::string escape_json(const std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char character : input) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output += character;
        }
    }
    return output;
}

std::optional<std::string> json_string(const std::string_view json, const std::string_view key) {
    const auto position = json_value_position(json, key);
    if (!position.has_value()) {
        return std::nullopt;
    }
    std::size_t value_start = *position;
    if (value_start >= json.size() || json[value_start] != '"') {
        return std::nullopt;
    }
    ++value_start;
    std::string value;
    bool escaped = false;
    for (std::size_t index = value_start; index < json.size(); ++index) {
        const char character = json[index];
        if (escaped) {
            switch (character) {
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                value += character;
                break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value += character;
        }
    }
    return std::nullopt;
}

std::optional<double> json_number(const std::string_view json, const std::string_view key) {
    const auto position = json_value_position(json, key);
    if (!position.has_value()) {
        return std::nullopt;
    }
    const char* begin = json.data() + *position;
    const char* end = json.data() + json.size();
    double value{};
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    const char* cursor = result.ptr;
    while (cursor < end &&
           (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) {
        ++cursor;
    }
    if (cursor < end && *cursor != ',' && *cursor != '}') {
        return std::nullopt;
    }
    return value;
}

void append_string(std::ostringstream& stream, const std::string_view key,
                   const std::string_view value) {
    if (!value.empty()) {
        stream << ",\"" << key << "\":\"" << escape_json(value) << '"';
    }
}

} // namespace

std::string EventEnvelope::encode() const {
    std::ostringstream stream;
    stream.precision(12);
    stream << event_prefix << "{\"event\":\"" << escape_json(event) << '"';
    append_string(stream, "task_id", task_id);
    append_string(stream, "run_id", run_id);
    append_string(stream, "task", task_kind);
    append_string(stream, "sample_id", sample_id);
    append_string(stream, "message", message);
    if (progress.has_value()) {
        stream << ",\"progress\":" << *progress;
    }
    for (const auto& [key, value] : artifact_uris) {
        append_string(stream, key, value);
    }
    if (proof_stress.has_value()) {
        stream << ",\"proof_stress\":" << *proof_stress;
    }
    append_string(stream, "result_json", result_json);
    stream << ",\"protocol_version\":" << protocol_version << '}';
    return stream.str();
}

std::optional<EventEnvelope> EventEnvelope::decode(const std::string_view line) {
    if (!line.starts_with(event_prefix)) {
        return std::nullopt;
    }
    const auto json = line.substr(event_prefix.size());
    const auto event_value = json_string(json, "event");
    const auto task_value = json_string(json, "task_id");
    const auto progress_value = json_number(json, "progress");
    const auto version_value = json_number(json, "protocol_version");
    if (!event_value || !version_value || *version_value != static_cast<double>(protocol_version) ||
        event_value->empty() || (progress_value.has_value() && !std::isfinite(*progress_value))) {
        return std::nullopt;
    }
    static constexpr std::string_view artifact_fields[]{
        "path", "output_dir", "odb_path", "animation_manifest", "result_path", "report_path"};
    std::map<std::string, std::string, std::less<>> artifacts;
    for (const auto field : artifact_fields) {
        if (const auto value = json_string(json, field); value.has_value() && !value->empty()) {
            artifacts.emplace(field, *value);
        }
    }
    return EventEnvelope{
        .event = *event_value,
        .task_id = task_value.value_or(""),
        .run_id = json_string(json, "run_id").value_or(""),
        .task_kind = json_string(json, "task").value_or(""),
        .sample_id = json_string(json, "sample_id").value_or(""),
        .message = json_string(json, "message").value_or(""),
        .progress = progress_value,
        .artifact_uris = std::move(artifacts),
        .proof_stress = json_number(json, "proof_stress"),
        .result_json = json_string(json, "result_json").value_or(""),
    };
}

} // namespace mbs::runtime
