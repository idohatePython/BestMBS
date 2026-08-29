#include "TestSupport.hpp"
#include "mbs/runtime/EventEnvelope.hpp"
#include "mbs/runtime/WorkerSession.hpp"

#include <string>

int main() {
    int failures = 0;
    const mbs::runtime::EventEnvelope original{
        .event = "progress",
        .task_id = "task-1",
        .run_id = "run-1",
        .task_kind = "simulation",
        .sample_id = "mbs-1",
        .message = "geometry \"ready\"\nline 2",
        .progress = 0.375,
        .artifact_uris = {{"odb_path", "job.odb"}, {"result_path", "proof.json"}},
        .proof_stress = 12.5,
        .result_json = "{\"proof_stress\":12.5}",
    };
    const std::string encoded = original.encode();
    MBS_CHECK(encoded.starts_with(mbs::runtime::event_prefix));
    const auto decoded = mbs::runtime::EventEnvelope::decode(encoded);
    MBS_CHECK(decoded.has_value());
    MBS_CHECK(decoded->event == original.event);
    MBS_CHECK(decoded->task_id == original.task_id);
    MBS_CHECK(decoded->run_id == original.run_id);
    MBS_CHECK(decoded->task_kind == original.task_kind);
    MBS_CHECK(decoded->sample_id == original.sample_id);
    MBS_CHECK(decoded->message == original.message);
    MBS_CHECK(decoded->progress == original.progress);
    MBS_CHECK(decoded->artifact_uris.at("odb_path") == "job.odb");
    MBS_CHECK(decoded->artifact_uris.at("result_path") == "proof.json");
    MBS_CHECK(decoded->proof_stress == 12.5);
    MBS_CHECK(decoded->result_json == original.result_json);
    MBS_CHECK(!mbs::runtime::EventEnvelope::decode("plain log").has_value());
    MBS_CHECK(!mbs::runtime::EventEnvelope::decode(
                   "TPMS_EVENT:{\"event\":\"x\",\"message\":\"\",\"progress\":0,"
                   "\"protocol_version\":2,\"task_id\":\"task\"}")
                   .has_value());
    const auto python_event = mbs::runtime::EventEnvelope::decode(
        "TPMS_EVENT:{\"event\": \"progress\", \"task_id\": \"python-task\", "
        "\"progress\": 50.0, \"protocol_version\": 1}");
    MBS_CHECK(python_event.has_value());
    MBS_CHECK(python_event->task_id == "python-task");
    MBS_CHECK(python_event->progress == 50.0);

    mbs::runtime::WorkerSession session{"task-1"};
    MBS_CHECK(session.consume("ordinary output").kind == mbs::runtime::WorkerLineKind::log);
    MBS_CHECK(session.consume(encoded).kind == mbs::runtime::WorkerLineKind::event);
    auto mismatched = original;
    mismatched.event = "completed";
    mismatched.task_id = "other-task";
    mismatched.progress = 1.0;
    MBS_CHECK(session.consume(mismatched.encode()).kind ==
              mbs::runtime::WorkerLineKind::rejected_event);
    auto completed = original;
    completed.event = "completed";
    completed.task_id = "task-1";
    completed.progress = 1.0;
    MBS_CHECK(session.consume(completed.encode()).kind == mbs::runtime::WorkerLineKind::event);
    MBS_CHECK(session.finish(0, false).status == mbs::runtime::WorkerTerminalStatus::succeeded);

    mbs::runtime::WorkerSession incomplete{"task-2"};
    MBS_CHECK(incomplete.finish(0, false).error == "Worker exited without a completion event");
    MBS_CHECK(incomplete.finish(130, true).status == mbs::runtime::WorkerTerminalStatus::cancelled);

    return failures == 0 ? 0 : 1;
}
