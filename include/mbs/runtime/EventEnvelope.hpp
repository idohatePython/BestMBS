#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mbs::runtime {

inline constexpr int protocol_version = 1;
inline constexpr std::string_view event_prefix = "TPMS_EVENT:";

struct EventEnvelope final {
    std::string event;
    std::string task_id;
    std::string run_id;
    std::string task_kind;
    std::string sample_id;
    std::string message;
    std::optional<double> progress;
    std::map<std::string, std::string, std::less<>> artifact_uris;
    std::optional<double> proof_stress;
    std::string result_json;

    [[nodiscard]] std::string encode() const;
    [[nodiscard]] static std::optional<EventEnvelope> decode(std::string_view line);
};

} // namespace mbs::runtime
