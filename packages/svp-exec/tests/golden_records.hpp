#pragma once

// Canonical bytes of the sample records in exec_test_support.hpp. The digests
// were computed with an independent pure-Python BLAKE3 over these exact
// strings, not with the library under test.

#include <string_view>

namespace svp::exec::test {

// BLAKE3 of {"case":"upper","ratio":0.5,"sample_indices":[120,121]}.
inline constexpr std::string_view kSampleParametersBlake3 =
    "c953ebfc09f55590e04b5aca9eb27be731f481d556e692676e242039ec627b8c";

inline constexpr std::string_view kSampleTaskSpecJson =
    R"({"build_session_id":"bs_0001",)"
    R"("cache_key":"b3:3333333333333333333333333333333333333333333333333333333333333333",)"
    R"("depends_on":["task.plan.ingest"],)"
    R"("inputs":{"source":{"blake3":"2222222222222222222222222222222222222222222222222222222222222222",)"
    R"("bytes":11,"media_type":"text/plain","role":"source_text"}},)"
    R"("model_refs":[{"bundle_blake3":"1111111111111111111111111111111111111111111111111111111111111111",)"
    R"("model_bundle_id":"model_toy_upper@1.0.0+blake3_111111111111","model_id":"model_toy_upper"}],)"
    R"("parameters":{"case":"upper","ratio":0.5,"sample_indices":[120,121]},)"
    R"("parameters_blake3":"c953ebfc09f55590e04b5aca9eb27be731f481d556e692676e242039ec627b8c",)"
    R"("resources":{"est_cpu_threads":1,"est_peak_rss_mb":64,"est_seconds":2},)"
    R"("schema":"svp-task-spec-v1","task_id":"task.toy.upper.chunk_000",)"
    R"("task_type":"toy.upper","task_type_version":1})";

// BLAKE3 of ["svp-task-output-digest-v1",[<the sample output ArtifactRef>]].
inline constexpr std::string_view kSampleOutputDigest =
    "cd518ce8ccd6180b50b198e927e17cca894cf9b8f993e238db85d49fcba261cb";

inline constexpr std::string_view kSampleTaskResultJson =
    R"({"attempt":1,"diagnostics":{"box_count":146},)"
    R"("execution":{"cpu_ms":{"system":812,"user":51234},"peak_rss_bytes":1221541888,)"
    R"("runtime_id":"b3:4444444444444444444444444444444444444444444444444444444444444444",)"
    R"("timing_ms":{"compute":9850,"decode":1310,"encode":4,"input_fetch":0,"queue":3},)"
    R"("worker_session_id":"ws_0001"},)"
    R"("output_digest":"b3:cd518ce8ccd6180b50b198e927e17cca894cf9b8f993e238db85d49fcba261cb",)"
    R"("outputs":[{"blake3":"5555555555555555555555555555555555555555555555555555555555555555",)"
    R"("bytes":48213,"media_type":"application/x-ndjson","role":"ocr_frame_detections"}],)"
    R"("schema":"svp-task-result-v1","status":"succeeded","task_id":"task.toy.upper.chunk_000"})";

}  // namespace svp::exec::test
