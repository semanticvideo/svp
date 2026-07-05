#include "audio_test_support.hpp"

void test_asr_chunk_plan_produces_correct_overlapping_chunks() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(65000000, 30000000, 5000000);
  const nlohmann::json encoded = svp::audio::asr_chunk_plan_to_json(plan);

  assert(encoded["chunk_count"] == 3);
  assert(encoded["chunk_duration_us"] == 30000000);
  assert(encoded["overlap_us"] == 5000000);
  assert(encoded["total_duration_us"] == 65000000);

  assert(plan.chunks.size() == 3);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 30000000);
  assert(plan.chunks[0].overlap_before_us == 0);
  assert(plan.chunks[0].overlap_after_us == 5000000);
  assert(plan.chunks[0].chunk_id == "asr_chunk_000000");
  assert(plan.chunks[0].output_ref == "transcript/words.chunk_000000.jsonl");

  assert(plan.chunks[1].source_start_us == 25000000);
  assert(plan.chunks[1].source_end_us == 55000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);
  assert(plan.chunks[1].overlap_after_us == 5000000);

  assert(plan.chunks[2].source_start_us == 50000000);
  assert(plan.chunks[2].source_end_us == 65000000);
  assert(plan.chunks[2].overlap_before_us == 5000000);
  assert(plan.chunks[2].overlap_after_us == 0);
}

void test_asr_chunk_plan_zero_duration_produces_no_chunks() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(0, 30000000, 5000000);
  assert(plan.chunks.empty());
}

void test_asr_chunk_plan_rejects_bad_parameters() {
  bool threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(-1, 30000000, 5000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);

  threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(1000000, 0, 5000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);

  threw = false;
  try {
    (void)svp::audio::build_asr_chunk_plan(1000000, 30000000, 30000000);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

void test_asr_chunk_plan_exact_multiple_has_no_trailing_chunk() {
  const svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(60000000, 30000000, 0);
  assert(plan.chunks.size() == 2);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 30000000);
  assert(plan.chunks[1].source_start_us == 30000000);
  assert(plan.chunks[1].source_end_us == 60000000);
  assert(plan.chunks[1].overlap_before_us == 0);
  assert(plan.chunks[1].overlap_after_us == 0);
}

void test_overlap_reconciliation_deduplicates_boundary_words() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks.size() == 2);
  assert(plan.chunks[0].source_start_us == 0);
  assert(plan.chunks[0].source_end_us == 20000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].source_end_us == 30000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);

  chunk_words[0].push_back({"hello", 1000000, 1500000, 0.9, 0});
  chunk_words[0].push_back({"world", 16000000, 16500000, 0.9, 0});
  chunk_words[0].push_back({"overlap_word", 17000000, 17500000, 0.85, 0});

  chunk_words[1].push_back({"overlap_word", 17000000 - 15000000, 17500000 - 15000000, 0.85, 1});
  chunk_words[1].push_back({"final", 25000000 - 15000000, 26000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 4);
  assert(reconciled[0].text == "hello");
  assert(reconciled[0].start_us == 1000000);
  assert(reconciled[1].text == "world");
  assert(reconciled[1].start_us == 16000000);
  assert(reconciled[2].text == "overlap_word");
  assert(reconciled[2].start_us == 17000000);
  assert(reconciled[3].text == "final");
  assert(reconciled[3].start_us == 25000000);
}

void test_overlap_reconciliation_duplicate_in_actual_overlap_region() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"alpha", 16000000, 16500000, 0.9, 0});
  chunk_words[1].push_back({"alpha", 16000000 - 15000000, 16500000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"beta", 22000000 - 15000000, 23000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "alpha");
  assert(reconciled[0].start_us == 16000000);
  assert(reconciled[1].text == "beta");
  assert(reconciled[1].start_us == 22000000);
}

void test_overlap_reconciliation_word_start_equals_prior_end_at_boundary() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"first", 16000000, 17000000, 0.9, 0});
  chunk_words[1].push_back({"first", 16000000 - 15000000, 17000000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"second", 17000000 - 15000000, 18000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "first");
  assert(reconciled[0].start_us == 16000000);
  assert(reconciled[0].end_us == 17000000);
  assert(reconciled[1].text == "second");
  assert(reconciled[1].start_us == 17000000);
  assert(reconciled[1].end_us == 18000000);
}

void test_overlap_reconciliation_shifted_token_inside_overlap() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"original", 17000000, 17500000, 0.9, 0});
  chunk_words[1].push_back({"shifted", 17200000 - 15000000, 17700000 - 15000000, 0.85, 1});
  chunk_words[1].push_back({"after", 21000000 - 15000000, 22000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 2);
  assert(reconciled[0].text == "original");
  assert(reconciled[0].start_us == 17000000);
  assert(reconciled[1].text == "after");
  assert(reconciled[1].start_us == 21000000);
}

void test_overlap_reconciliation_no_false_dedepe_outside_overlap() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  assert(plan.chunks[1].source_start_us == 15000000);
  assert(plan.chunks[1].overlap_before_us == 5000000);

  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  chunk_words[0].push_back({"early", 1000000, 2000000, 0.9, 0});
  chunk_words[0].push_back({"mid", 12000000, 13000000, 0.9, 0});
  chunk_words[1].push_back({"mid_dup", 12000000 - 15000000, 13000000 - 15000000, 0.9, 1});
  chunk_words[1].push_back({"late", 25000000 - 15000000, 26000000 - 15000000, 0.9, 1});

  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);

  assert(reconciled.size() == 3);
  assert(reconciled[0].text == "early");
  assert(reconciled[0].start_us == 1000000);
  assert(reconciled[1].text == "mid");
  assert(reconciled[1].start_us == 12000000);
  assert(reconciled[2].text == "late");
  assert(reconciled[2].start_us == 25000000);
}

void test_overlap_reconciliation_empty_chunks() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);
  std::vector<std::vector<svp::audio::AsrWord>> chunk_words(2);
  std::vector<svp::audio::AsrWord> reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);
  assert(reconciled.empty());
}

void test_asr_model_present_vs_verified_distinction() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "svp-asr-model-verify-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "model_whisper_small_en");

  const std::string model_id = "model_whisper_small_en";

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == false);
  assert(svp::audio::verify_asr_model_files(model_id, root) == false);

  {
    std::ofstream manifest(root / "model_whisper_small_en" / "model.svpmodel.json");
    manifest << R"({"schema_version":"svp-model-bundle-1",)"
             << R"("model_bundle_id":"model_whisper_small_en@v1+blake3_000000000000",)"
             << R"("model_id":")" << model_id << R"(","model_version":"v1",)"
             << R"("bundle_blake3":"blake3:0000000000000000000000000000000000000000000000000000000000000000",)"
             << R"("runtime":"onnxruntime","format":"onnx","license":"MIT",)"
             << R"("supported_execution_providers":["cpu"],)"
             << R"("files":[{"path":"model.onnx","role":"model",)"
             << R"("blake3":"blake3:0000000000000000000000000000000000000000000000000000000000000000"}],)"
             << R"("input_contract":{},"output_contract":{},)"
             << R"("preprocessor_contract":{},"postprocessor_contract":{}})";
  }

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == true);
  assert(svp::audio::verify_asr_model_files(model_id, root) == false);

  {
    std::ofstream model_file(root / "model_whisper_small_en" / "model.onnx");
    model_file << "dummy";
  }

  assert(svp::audio::check_asr_model_in_cache(model_id, root) == true);
  assert(svp::audio::verify_asr_model_files(model_id, root) == true);

  std::filesystem::remove_all(root);
}

void test_asr_execution_boundary_blocked_when_model_missing() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, false, false);
  const nlohmann::json encoded =
      svp::audio::asr_execution_boundary_to_json(boundary);

  assert(encoded["asr_status"] == "blocked");
  assert(encoded["analysis_audio_available"] == true);
  assert(encoded["model_runtime_available"] == true);
  assert(encoded["model_available"] == false);
  assert(encoded["model_verified"] == false);
  assert(!encoded["blockers"].empty());
}

void test_asr_execution_boundary_blocked_when_runtime_missing() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, false, false, false);
  assert(boundary.asr_status == svp::audio::AsrStatus::blocked);
  assert(!boundary.blockers.empty());
}

void test_asr_execution_boundary_planned_when_all_available() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  assert(boundary.asr_status == svp::audio::AsrStatus::planned);
  assert(boundary.blockers.empty());
}

void test_asr_execution_boundary_json_reports_decoder_token_softmax_mean() {
  svp::audio::AsrChunkPlanResult plan =
      svp::audio::build_asr_chunk_plan(30000000, 20000000, 5000000);

  svp::audio::AsrExecutionBoundary boundary =
      svp::audio::build_asr_execution_boundary(plan, true, true, true, true);
  const nlohmann::json encoded =
      svp::audio::asr_execution_boundary_to_json(boundary);

  assert(encoded["asr_limitations"]["confidence_status"] == "decoder_token_softmax_mean");
  std::string note = encoded["asr_limitations"]["confidence_note"];
  assert(note.find("uncalibrated") != std::string::npos);
}
