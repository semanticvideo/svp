#include "exec_test_support.hpp"
#include "golden_records.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <limits>
#include <string>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

// Decodes `json` after editing it, re-encoding canonically so only the edit
// (not formatting) is under test.
template <typename Edit>
TaskSpec decode_edited_spec(Edit&& edit) {
  nlohmann::json value = nlohmann::json::parse(kSampleTaskSpecJson);
  edit(value);
  return decode_task_spec(value.dump());
}

template <typename Edit>
TaskResult decode_edited_result(Edit&& edit) {
  nlohmann::json value = nlohmann::json::parse(kSampleTaskResultJson);
  edit(value);
  return decode_task_result(value.dump());
}

void test_artifact_ref_round_trip() {
  const ArtifactRef ref = make_artifact_ref(to_bytes("hello world"), "text/plain",
                                            "source_text");
  expect(ref.bytes == 11, "make_artifact_ref length");
  const nlohmann::json value = artifact_ref_to_json(ref);
  expect_equal(encode_canonical_json(value),
               R"({"blake3":")" + blake3_hex(ref.blake3) +
                   R"(","bytes":11,"media_type":"text/plain","role":"source_text"})",
               "ArtifactRef canonical form");
  expect(artifact_ref_from_json(value) == ref, "ArtifactRef round trip");
  // In-process JSON built from signed literals is accepted when non-negative.
  const nlohmann::json signed_literal = {{"blake3", blake3_hex(ref.blake3)},
                                         {"bytes", 11},
                                         {"media_type", "text/plain"},
                                         {"role", "source_text"}};
  expect(artifact_ref_from_json(signed_literal) == ref,
         "ArtifactRef from signed integer literal");

  nlohmann::json missing_media = value;
  missing_media.erase("media_type");
  expect_exec_error(ExecErrorCode::missing_field,
                    [&] { static_cast<void>(artifact_ref_from_json(missing_media)); },
                    "ArtifactRef without media_type");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] {
                      static_cast<void>(make_artifact_ref({}, "Text/Plain", "x"));
                    },
                    "upper-case media type");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { static_cast<void>(make_artifact_ref({}, "text/plain", "")); },
                    "empty role");
}

void test_task_spec_golden_round_trip() {
  const TaskSpec spec = sample_task_spec();
  expect_equal(blake3_hex(spec.parameters_blake3), kSampleParametersBlake3,
               "parameters_blake3 test vector");
  const std::string bytes = encode_task_spec(spec);
  expect_equal(bytes, kSampleTaskSpecJson, "TaskSpec canonical bytes");
  const TaskSpec decoded = decode_task_spec(bytes);
  expect(decoded == spec, "TaskSpec round trip");
  expect_equal(encode_task_spec(decoded), bytes, "TaskSpec re-encode is stable");
}

void test_task_result_golden_round_trip() {
  const TaskResult result = sample_task_result();
  expect_equal(blake3_hex(result.output_digest), kSampleOutputDigest,
               "output_digest test vector");
  const std::string bytes = encode_task_result(result);
  expect_equal(bytes, kSampleTaskResultJson, "TaskResult canonical bytes");
  const TaskResult decoded = decode_task_result(bytes);
  expect(decoded == result, "TaskResult round trip");
  expect_equal(encode_task_result(decoded), bytes, "TaskResult re-encode is stable");
}

void test_failed_task_result_round_trip() {
  TaskResult failed = sample_task_result();
  failed.status = TaskStatus::failed;
  failed.outputs.clear();
  failed.output_digest = compute_output_digest(failed.outputs);
  failed.error = TaskError{
      .code = "insufficient_memory", .message = "RSS budget exceeded", .retryable = true};
  const std::string bytes = encode_task_result(failed);
  expect(bytes.find(R"("error":{"code":"insufficient_memory","message":"RSS budget exceeded","retryable":true})") !=
             std::string::npos,
         "failed result carries its error");
  expect(decode_task_result(bytes) == failed, "failed TaskResult round trip");

  TaskResult without_error = failed;
  without_error.error.reset();
  expect_exec_error(ExecErrorCode::missing_field,
                    [&] { static_cast<void>(encode_task_result(without_error)); },
                    "failed result without error");
  TaskResult failed_with_outputs = sample_task_result();
  failed_with_outputs.status = TaskStatus::failed;
  failed_with_outputs.error = failed.error;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(encode_task_result(failed_with_outputs)); },
                    "failed result with outputs");
  TaskResult succeeded_with_error = sample_task_result();
  succeeded_with_error.error = failed.error;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(encode_task_result(succeeded_with_error)); },
                    "succeeded result with error");
}

void test_unknown_fields_rejected() {
  expect_exec_error(ExecErrorCode::unknown_field,
                    [] { decode_edited_spec([](auto& v) { v["priority"] = 1; }); },
                    "unknown top-level TaskSpec field");
  expect_exec_error(ExecErrorCode::unknown_field,
                    [] {
                      decode_edited_spec(
                          [](auto& v) { v["inputs"]["source"]["path"] = "/tmp/x"; });
                    },
                    "unknown ArtifactRef field (no paths on the wire)");
  expect_exec_error(ExecErrorCode::unknown_field,
                    [] { decode_edited_spec([](auto& v) { v["resources"]["gpu"] = 1; }); },
                    "unknown resources field");
  expect_exec_error(ExecErrorCode::unknown_field,
                    [] {
                      decode_edited_result(
                          [](auto& v) { v["execution"]["timing_ms"]["wait"] = 1; });
                    },
                    "unknown timing field");
  expect_exec_error(ExecErrorCode::unknown_field,
                    [] { decode_edited_result([](auto& v) { v["hostname"] = "x"; }); },
                    "unknown TaskResult field");
}

void test_missing_fields_rejected() {
  for (const char* field :
       {"build_session_id", "cache_key", "depends_on", "inputs", "model_refs",
        "parameters", "parameters_blake3", "resources", "schema", "task_id",
        "task_type", "task_type_version"}) {
    expect_exec_error(ExecErrorCode::missing_field,
                      [field] { decode_edited_spec([field](auto& v) { v.erase(field); }); },
                      std::string("TaskSpec without ") + field);
  }
  for (const char* field : {"attempt", "diagnostics", "execution", "output_digest",
                            "outputs", "schema", "status", "task_id"}) {
    expect_exec_error(ExecErrorCode::missing_field,
                      [field] {
                        decode_edited_result([field](auto& v) { v.erase(field); });
                      },
                      std::string("TaskResult without ") + field);
  }
  expect_exec_error(ExecErrorCode::missing_field,
                    [] {
                      decode_edited_result(
                          [](auto& v) { v["execution"].erase("peak_rss_bytes"); });
                    },
                    "execution without peak_rss_bytes");
}

void test_bad_digests_rejected() {
  const std::string upper(64, 'A');
  const std::string non_hex(64, 'z');
  const std::string short_hex(62, 'a');
  const std::string long_hex(66, 'a');
  for (const std::string& bad : {upper, non_hex, short_hex, long_hex}) {
    expect_exec_error(ExecErrorCode::invalid_digest,
                      [&] {
                        decode_edited_spec(
                            [&](auto& v) { v["inputs"]["source"]["blake3"] = bad; });
                      },
                      "bad ArtifactRef blake3: " + bad);
    expect_exec_error(ExecErrorCode::invalid_digest,
                      [&] { decode_edited_spec([&](auto& v) { v["cache_key"] = "b3:" + bad; }); },
                      "bad cache_key: " + bad);
    expect_exec_error(ExecErrorCode::invalid_digest,
                      [&] {
                        decode_edited_result(
                            [&](auto& v) { v["execution"]["runtime_id"] = "b3:" + bad; });
                      },
                      "bad runtime_id: " + bad);
  }
  expect_exec_error(ExecErrorCode::invalid_digest,
                    [] {
                      decode_edited_spec([](auto& v) {
                        v["cache_key"] = std::string(64, '3');
                      });
                    },
                    "cache_key without b3: prefix");
  expect_exec_error(ExecErrorCode::invalid_digest,
                    [] {
                      decode_edited_spec([](auto& v) {
                        v["parameters_blake3"] = "b3:" + std::string(kSampleParametersBlake3);
                      });
                    },
                    "parameters_blake3 with a prefix");
}

void test_digest_consistency_enforced() {
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [] {
                      decode_edited_spec([](auto& v) { v["parameters"]["ratio"] = 0.25; });
                    },
                    "parameters edited without updating parameters_blake3");
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [] {
                      decode_edited_result(
                          [](auto& v) { v["outputs"][0]["bytes"] = 48214; });
                    },
                    "outputs edited without updating output_digest");
}

void test_wrong_types_and_values_rejected() {
  expect_exec_error(ExecErrorCode::wrong_type,
                    [] { decode_edited_spec([](auto& v) { v["task_type_version"] = 1.0; }); },
                    "float where an integer is required");
  expect_exec_error(ExecErrorCode::wrong_type,
                    [] {
                      decode_edited_spec([](auto& v) { v["inputs"]["source"]["bytes"] = -1; });
                    },
                    "negative byte count");
  expect_exec_error(ExecErrorCode::wrong_type,
                    [] { decode_edited_spec([](auto& v) { v["parameters"] = nlohmann::json::array(); }); },
                    "array parameters");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { decode_edited_spec([](auto& v) { v["schema"] = "svp-task-spec-v2"; }); },
                    "wrong TaskSpec schema");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { decode_edited_spec([](auto& v) { v["task_type_version"] = 0; }); },
                    "task_type_version 0");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { decode_edited_spec([](auto& v) { v["task_id"] = "task with spaces"; }); },
                    "task_id with spaces");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] {
                      decode_edited_spec([](auto& v) {
                        v["depends_on"] = nlohmann::json::array({"task.b", "task.a"});
                      });
                    },
                    "unsorted depends_on");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] {
                      decode_edited_spec([](auto& v) {
                        v["depends_on"] = nlohmann::json::array({"task.a", "task.a"});
                      });
                    },
                    "duplicate depends_on");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] {
                      decode_edited_spec([](auto& v) {
                        v["model_refs"][0]["model_bundle_id"] =
                            "model_toy_upper@1.0.0+blake3_222222222222";
                      });
                    },
                    "model_bundle_id hash prefix disagrees with bundle_blake3");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { decode_edited_result([](auto& v) { v["status"] = "running"; }); },
                    "unknown status");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { decode_edited_result([](auto& v) { v["attempt"] = 0; }); },
                    "attempt 0");
}

void test_non_canonical_and_non_finite_rejected() {
  std::string spaced(kSampleTaskSpecJson);
  spaced.insert(1, " ");
  expect_exec_error(ExecErrorCode::non_canonical_json,
                    [&] { static_cast<void>(decode_task_spec(spaced)); },
                    "TaskSpec with whitespace");
  TaskSpec spec = sample_task_spec();
  spec.parameters["threshold"] = std::numeric_limits<double>::infinity();
  expect_exec_error(ExecErrorCode::non_finite_number,
                    [&] { static_cast<void>(encode_task_spec(spec)); },
                    "TaskSpec with an infinite parameter");
  TaskResult result = sample_task_result();
  result.diagnostics["score"] = std::numeric_limits<double>::quiet_NaN();
  expect_exec_error(ExecErrorCode::non_finite_number,
                    [&] { static_cast<void>(encode_task_result(result)); },
                    "TaskResult with a NaN diagnostic");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec record tests",
      {{"artifact_ref_round_trip", test_artifact_ref_round_trip},
       {"task_spec_golden_round_trip", test_task_spec_golden_round_trip},
       {"task_result_golden_round_trip", test_task_result_golden_round_trip},
       {"failed_task_result_round_trip", test_failed_task_result_round_trip},
       {"unknown_fields_rejected", test_unknown_fields_rejected},
       {"missing_fields_rejected", test_missing_fields_rejected},
       {"bad_digests_rejected", test_bad_digests_rejected},
       {"digest_consistency_enforced", test_digest_consistency_enforced},
       {"wrong_types_and_values_rejected", test_wrong_types_and_values_rejected},
       {"non_canonical_and_non_finite_rejected",
        test_non_canonical_and_non_finite_rejected}});
}
