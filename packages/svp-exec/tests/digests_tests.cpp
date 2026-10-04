#include "exec_test_support.hpp"
#include "golden_records.hpp"
#include "svp/exec/cache_key.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/exec/processor_cache_keys.hpp"

#include <string>
#include <vector>

// Every expected digest below was computed with an independent pure-Python
// BLAKE3 implementation over the documented material string.

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

Blake3Digest sample_parameters_blake3() {
  return *parse_blake3_hex(kSampleParametersBlake3);
}

Blake3Digest digest_with_prefix(std::string_view prefix) {
  std::string hex(prefix);
  hex.resize(kBlake3HexChars, '0');
  return *parse_blake3_hex(hex);
}

void test_parameters_blake3_vector() {
  // Key order in the source object does not matter: the digest is taken over
  // the canonical encoding.
  const nlohmann::json parameters = nlohmann::json::parse(
      R"({"sample_indices":[120,121],"ratio":0.5,"case":"upper"})");
  expect_equal(blake3_hex(compute_parameters_blake3(parameters)),
               kSampleParametersBlake3, "parameters_blake3 vector");
  nlohmann::json changed = parameters;
  changed["sample_indices"][1] = 122;
  expect(compute_parameters_blake3(changed) != sample_parameters_blake3(),
         "a changed parameter changes parameters_blake3");
  expect_exec_error(ExecErrorCode::wrong_type,
                    [] {
                      static_cast<void>(
                          compute_parameters_blake3(nlohmann::json::array()));
                    },
                    "array parameters");
}

void test_generic_cache_key_vector() {
  const nlohmann::json fields =
      nlohmann::json::array({"task.toy.upper", 1, std::string(kSampleParametersBlake3)});
  expect_equal(cache_key_material(fields),
               R"(["svp-cache-key-v1","task.toy.upper",1,")" +
                   std::string(kSampleParametersBlake3) + R"("])",
               "generic cache key material");
  expect_equal(blake3_prefixed(compute_cache_key(fields)),
               "b3:763602ec4391c1a910247b4c0a59283d4e401258b16709e3d648ed8601e006dc",
               "generic cache key vector");
  // Field types are part of the key: "1" and 1 differ.
  const nlohmann::json string_version =
      nlohmann::json::array({"task.toy.upper", "1", std::string(kSampleParametersBlake3)});
  expect(compute_cache_key(string_version) != compute_cache_key(fields),
         "cache key ignores field types");
  expect_exec_error(ExecErrorCode::wrong_type,
                    [] {
                      static_cast<void>(compute_cache_key(nlohmann::json::object()));
                    },
                    "object cache key fields");
}

CacheKeyModelIdentity depth_model() {
  return CacheKeyModelIdentity{
      .model_id = "model_depth_anything_v2_small",
      .model_bundle_id = "model_depth_anything_v2_small@2024-06-13+blake3_8f4c21aa93d0",
      .bundle_blake3 = digest_with_prefix("8f4c21aa93d0"),
      .model_blake3 = repeated_digest(0x66),
      .model_runtime = "onnxruntime",
      .execution_provider = "cpu"};
}

VisualCacheKeyInputs sample_visual_inputs() {
  return VisualCacheKeyInputs{
      .svp_core_schema_version = "1.0-rc2",
      .processor_id = "proc_depth_0001",
      .processor_version = "1.0.0",
      .model = depth_model(),
      .normalized_input_format = "rgb24_518x518",
      .time_range = CacheKeyTimeRangeUs{.start_us = 5020000, .end_us = 5080000},
      .frame_indices = {120480, 121920},
      .decoded_pixel_blake3 = repeated_digest(0x77),
      .processor_parameters_blake3 = sample_parameters_blake3()};
}

void test_visual_cache_key_vector() {
  const VisualCacheKeyInputs inputs = sample_visual_inputs();
  expect_equal(
      cache_key_material(visual_cache_key_fields(inputs)),
      R"(["svp-cache-key-v1","1.0-rc2","proc_depth_0001","1.0.0",)"
      R"("model_depth_anything_v2_small",)"
      R"("model_depth_anything_v2_small@2024-06-13+blake3_8f4c21aa93d0",)"
      R"("8f4c21aa93d00000000000000000000000000000000000000000000000000000",)"
      R"("6666666666666666666666666666666666666666666666666666666666666666",)"
      R"("onnxruntime","cpu","rgb24_518x518",[5020000,5080000],[120480,121920],)"
      R"("7777777777777777777777777777777777777777777777777777777777777777",)"
      R"("c953ebfc09f55590e04b5aca9eb27be731f481d556e692676e242039ec627b8c"])",
      "visual cache key material follows RC2 §20.3 field order");
  expect_equal(blake3_prefixed(visual_cache_key(inputs)),
               "b3:d098c170a6ef3772e674cce40bae815b097e8004847f5d0f1868fc99a04e1df1",
               "visual cache key vector");

  VisualCacheKeyInputs other_frames = inputs;
  other_frames.frame_indices = {120480, 121921};
  expect(visual_cache_key(other_frames) != visual_cache_key(inputs),
         "frame indices are part of the visual key");
  VisualCacheKeyInputs backwards = inputs;
  backwards.time_range = CacheKeyTimeRangeUs{.start_us = 10, .end_us = 5};
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(visual_cache_key(backwards)); },
                    "time range ending before it starts");
  VisualCacheKeyInputs no_provider = inputs;
  no_provider.model.execution_provider.clear();
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(visual_cache_key(no_provider)); },
                    "empty execution provider");
}

void test_audio_cache_key_vector() {
  const AudioCacheKeyInputs inputs{
      .svp_core_schema_version = "1.0-rc2",
      .processor_id = "proc_asr_0001",
      .processor_version = "1.0.0",
      .model = CacheKeyModelIdentity{
          .model_id = "model_whisper_small",
          .model_bundle_id = "model_whisper_small@1.0.0+blake3_999999999999",
          .bundle_blake3 = repeated_digest(0x99),
          .model_blake3 = repeated_digest(0xaa),
          .model_runtime = "whisper.cpp",
          .execution_provider = "cpu"},
      .sample_rate = 16000,
      .channel_layout = "mono",
      .sample_format = "s16le",
      .audio_chunk_blake3 = repeated_digest(0xbb),
      .processor_parameters_blake3 = sample_parameters_blake3()};
  expect_equal(
      cache_key_material(audio_cache_key_fields(inputs)),
      R"(["svp-cache-key-v1","1.0-rc2","proc_asr_0001","1.0.0","model_whisper_small",)"
      R"("model_whisper_small@1.0.0+blake3_999999999999",)"
      R"("9999999999999999999999999999999999999999999999999999999999999999",)"
      R"("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
      R"("whisper.cpp","cpu",16000,"mono","s16le",)"
      R"("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)"
      R"("c953ebfc09f55590e04b5aca9eb27be731f481d556e692676e242039ec627b8c"])",
      "audio cache key material follows RC2 §20.3 field order");
  expect_equal(blake3_prefixed(audio_cache_key(inputs)),
               "b3:db25ac4532b4a8569525508b2e14bb09d5ea72093f7336e8859b1b553c2c3d65",
               "audio cache key vector");
}

void test_output_digest_vectors() {
  const TaskResult result = sample_task_result();
  expect_equal(output_digest_material(result.outputs),
               R"(["svp-task-output-digest-v1",[{"blake3":")" +
                   std::string(64, '5') +
                   R"(","bytes":48213,"media_type":"application/x-ndjson",)"
                   R"("role":"ocr_frame_detections"}]])",
               "output digest material");
  expect_equal(blake3_hex(compute_output_digest(result.outputs)), kSampleOutputDigest,
               "output digest vector");
  expect_equal(blake3_hex(compute_output_digest({})),
               "451e4c652aad313a06efd44921e5e53c44e2f187c8fadde8fd45c4e36b9200b4",
               "empty output list digest vector");

  std::vector<ArtifactRef> two = result.outputs;
  two.push_back(ArtifactRef{.blake3 = repeated_digest(0x66),
                            .bytes = 1,
                            .media_type = "application/octet-stream",
                            .role = "extra"});
  std::vector<ArtifactRef> swapped = {two[1], two[0]};
  expect(compute_output_digest(two) != compute_output_digest(swapped),
         "output order is part of the digest");
}

// The BLAKE3 reference test vectors (test_vectors.json of the BLAKE3
// repository) for the empty input: its key and context strings, and the
// first 32 bytes of the keyed_hash and derive_key outputs.
void test_keyed_and_derive_key_reference_vectors() {
  constexpr std::string_view kKey = "whats the Elvish word for friend";
  constexpr std::string_view kContext = "BLAKE3 2019-12-27 16:29:52 test vectors context";
  Blake3Digest key{};
  for (std::size_t index = 0; index < key.size(); ++index) {
    key[index] = static_cast<std::uint8_t>(kKey[index]);
  }
  expect_equal(blake3_hex(blake3_keyed_hash(key, {})),
               std::string("92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26"),
               "keyed_hash of the empty input");
  expect_equal(blake3_hex(blake3_derive_key(kContext, {})),
               std::string("2cc39783c223154fea8dfb7c1b1660f2ac2dcbd1c1de8277b0b0dd39b7e50d7d"),
               "derive_key of the empty input");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec digest tests",
      {{"parameters_blake3_vector", test_parameters_blake3_vector},
       {"generic_cache_key_vector", test_generic_cache_key_vector},
       {"visual_cache_key_vector", test_visual_cache_key_vector},
       {"audio_cache_key_vector", test_audio_cache_key_vector},
       {"output_digest_vectors", test_output_digest_vectors},
       {"keyed_and_derive_key_reference_vectors", test_keyed_and_derive_key_reference_vectors}});
}
