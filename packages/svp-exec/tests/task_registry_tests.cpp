#include "exec_test_support.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/task_registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kToyType = "toy.upper";
constexpr std::string_view kSourceText = "hello world";

struct TemporaryDirectory {
  fs::path path;

  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("svp-exec-task-registry-tests-" + std::to_string(nonce));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

std::optional<std::string> validate_toy_parameters(const nlohmann::json& parameters) {
  if (parameters.size() != 1 || !parameters.contains("case")) {
    return "toy.upper takes exactly one parameter, `case`";
  }
  const nlohmann::json& text_case = parameters.at("case");
  if (!text_case.is_string() || (text_case != "upper" && text_case != "lower")) {
    return "`case` must be \"upper\" or \"lower\"";
  }
  return std::nullopt;
}

std::string transform(std::string text, bool upper) {
  std::transform(text.begin(), text.end(), text.begin(), [upper](unsigned char c) {
    return static_cast<char>(upper ? std::toupper(c) : std::tolower(c));
  });
  return text;
}

// Toy task: reads its `source` input and returns the case-converted text as
// its only output.
TaskResult execute_toy(const TaskSpec& spec, const ResolvedInputs& inputs) {
  std::ifstream input(inputs.at("source").path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::string output =
      transform(text, spec.parameters.at("case").get<std::string>() == "upper");

  TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = 1;
  result.status = TaskStatus::succeeded;
  result.outputs = {make_artifact_ref(to_bytes(output), "text/plain", "cased_text")};
  result.output_digest = compute_output_digest(result.outputs);
  result.execution = sample_execution();
  result.diagnostics = nlohmann::json{{"chars", output.size()}};
  return result;
}

TaskTypeDefinition toy_definition() {
  return TaskTypeDefinition{.name = std::string(kToyType),
                            .version = 1,
                            .validate_parameters = validate_toy_parameters,
                            .execute = execute_toy};
}

TaskSpec toy_spec(const nlohmann::json& parameters) {
  TaskSpec spec = sample_task_spec();
  spec.inputs.clear();
  spec.inputs.emplace("source", make_artifact_ref(to_bytes(kSourceText), "text/plain",
                                                  "source_text"));
  spec.parameters = parameters;
  spec.parameters_blake3 = compute_parameters_blake3(parameters);
  return spec;
}

ResolvedInputs resolve_source(const TaskSpec& spec, const fs::path& directory) {
  const fs::path path = directory / "source.txt";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << kSourceText;
  output.close();
  return ResolvedInputs{{"source", ResolvedInput{.ref = spec.inputs.at("source"),
                                                 .path = path}}};
}

void test_registration_rules() {
  TaskTypeRegistry registry;
  registry.register_type(toy_definition());
  expect(registry.find(kToyType, 1) != nullptr, "registered type is found");
  expect(registry.find(kToyType, 2) == nullptr, "other version is unknown");
  expect(registry.find("toy.lower", 1) == nullptr, "other name is unknown");

  expect_exec_error(ExecErrorCode::duplicate_task_type,
                    [&] { registry.register_type(toy_definition()); },
                    "duplicate registration");
  TaskTypeDefinition other_version = toy_definition();
  other_version.version = 2;
  expect_exec_error(ExecErrorCode::duplicate_task_type,
                    [&] { registry.register_type(other_version); },
                    "second version of a registered name");

  TaskTypeDefinition bad = toy_definition();
  bad.name = "Toy Upper";
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { registry.register_type(bad); }, "malformed name");
  bad = toy_definition();
  bad.name = "toy.zero";
  bad.version = 0;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { registry.register_type(bad); }, "version 0");
  bad = toy_definition();
  bad.name = "toy.noexec";
  bad.execute = nullptr;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { registry.register_type(bad); }, "missing execute function");
}

void test_admission_rules() {
  TaskTypeRegistry registry;
  registry.register_type(toy_definition());
  TemporaryDirectory directory;

  TaskSpec unknown = toy_spec(nlohmann::json{{"case", "upper"}});
  unknown.task_type = "toy.reverse";
  expect_exec_error(ExecErrorCode::unknown_task_type,
                    [&] { static_cast<void>(registry.admit(unknown)); },
                    "unknown task type");
  TaskSpec newer = toy_spec(nlohmann::json{{"case", "upper"}});
  newer.task_type_version = 2;
  expect_exec_error(ExecErrorCode::unknown_task_type,
                    [&] { static_cast<void>(registry.admit(newer)); },
                    "unregistered task type version");

  const TaskSpec bad_parameters = toy_spec(nlohmann::json{{"case", "title"}});
  expect_exec_error(ExecErrorCode::invalid_task_parameters,
                    [&] { static_cast<void>(registry.admit(bad_parameters)); },
                    "parameters rejected by the type's validator");

  TaskSpec stale_digest = toy_spec(nlohmann::json{{"case", "upper"}});
  stale_digest.parameters["case"] = "lower";
  expect_exec_error(ExecErrorCode::digest_mismatch,
                    [&] { static_cast<void>(registry.admit(stale_digest)); },
                    "parameters that no longer match parameters_blake3");

  const TaskSpec spec = toy_spec(nlohmann::json{{"case", "upper"}});
  ResolvedInputs wrong_ref = resolve_source(spec, directory.path);
  wrong_ref.at("source").ref.bytes += 1;
  expect_exec_error(ExecErrorCode::unresolved_input,
                    [&] { static_cast<void>(registry.execute(spec, wrong_ref)); },
                    "input resolved to a different artifact");
  expect_exec_error(ExecErrorCode::unresolved_input,
                    [&] { static_cast<void>(registry.execute(spec, {})); },
                    "missing resolved input");
}

void test_toy_task_executes() {
  TaskTypeRegistry registry;
  registry.register_type(toy_definition());
  TemporaryDirectory directory;

  const TaskSpec spec = toy_spec(nlohmann::json{{"case", "upper"}});
  // The spec survives the wire before it runs, as it would on a worker.
  const TaskSpec received = decode_task_spec(encode_task_spec(spec));
  const TaskResult result =
      registry.execute(received, resolve_source(received, directory.path));

  const std::vector<std::byte> expected_output = to_bytes("HELLO WORLD");
  expect(result.status == TaskStatus::succeeded, "toy task succeeded");
  expect(result.outputs.size() == 1, "one output");
  expect(result.outputs.front().blake3 == blake3_digest(expected_output),
         "output bytes are the upper-cased input");
  expect(result.output_digest == compute_output_digest(result.outputs),
         "output_digest verifies");

  // The result survives the wire with its payload and still verifies.
  FrameDecoder decoder;
  decoder.feed(encode_frame(make_result_frame(result, {expected_output})));
  const auto frame = decoder.next();
  expect(frame.has_value(), "RESULT frame decodes");
  const TaskResult returned = task_result_from_result_frame(*frame);
  expect(returned == result, "TaskResult survives the RESULT frame");
  expect(returned.output_digest == compute_output_digest(returned.outputs),
         "received output_digest verifies");
}

void test_result_for_another_task_rejected() {
  TaskTypeRegistry registry;
  TaskTypeDefinition confused = toy_definition();
  confused.execute = [](const TaskSpec& spec, const ResolvedInputs& inputs) {
    TaskResult result = execute_toy(spec, inputs);
    result.task_id = "task.someone.else";
    return result;
  };
  registry.register_type(confused);
  TemporaryDirectory directory;
  const TaskSpec spec = toy_spec(nlohmann::json{{"case", "lower"}});
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] {
                      static_cast<void>(
                          registry.execute(spec, resolve_source(spec, directory.path)));
                    },
                    "result for a different task_id");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec task registry tests",
      {{"registration_rules", test_registration_rules},
       {"admission_rules", test_admission_rules},
       {"toy_task_executes", test_toy_task_executes},
       {"result_for_another_task_rejected", test_result_for_another_task_rejected}});
}
