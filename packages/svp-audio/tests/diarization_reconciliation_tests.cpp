#include "audio_test_support.hpp"

void test_reconcile_single_pair_low_similarity_keeps_two_speakers() {
  // Two clusters with low similarity (0.2) should remain 2 speakers.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.2f},
      {0.2f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 2);
  assert(result.merge_decisions.size() == 1);
  assert(result.merge_decisions[0].merged == false);
  assert(result.merge_decisions[0].cosine_similarity == 0.2f);
  assert(result.cluster_to_final.at(0) != result.cluster_to_final.at(1));
}

void test_reconcile_single_pair_high_similarity_merges_to_one() {
  // Two clusters with high similarity (0.8) should merge to 1 speaker.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.8f},
      {0.8f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 1);
  assert(result.merge_decisions.size() == 1);
  assert(result.merge_decisions[0].merged == true);
  assert(result.merge_decisions[0].cosine_similarity == 0.8f);
  assert(result.cluster_to_final.at(0) == result.cluster_to_final.at(1));
}

void test_reconcile_three_cluster_largest_gap_keeps_two_speakers() {
  // 3 clusters with similarities [0.456, 0.101, -0.045].
  // Largest gap = 0.356 (between 0.456 and 0.101), above 0.25 min-gap.
  // Clusters 0&1 merge, cluster 2 stays separate -> 2 final speakers.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.456f, 0.101f},
      {0.456f, 1.0f, -0.045f},
      {0.101f, -0.045f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1, 2};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 2);
  assert(result.merge_decisions.size() == 3);
  // Highest sim pair (0&1, sim=0.456) should merge
  bool found_merge = false;
  bool found_no_merge = false;
  for (const auto& md : result.merge_decisions) {
    if (md.merged) found_merge = true;
    if (!md.merged) found_no_merge = true;
  }
  assert(found_merge);
  assert(found_no_merge);
}

void test_reconcile_three_cluster_min_gap_merges_all_to_one() {
  // 3 clusters with similarities [0.649, 0.471, 0.343].
  // Largest gap = 0.178, below 0.25 min-gap threshold.
  // All clusters merge -> 1 final speaker.
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.471f, 0.649f},
      {0.471f, 1.0f, 0.343f},
      {0.649f, 0.343f, 1.0f}
  };
  std::vector<int32_t> cluster_ids = {0, 1, 2};

  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);

  assert(result.final_speaker_count == 1);
  assert(result.merge_decisions.size() == 3);
  for (const auto& md : result.merge_decisions) {
    assert(md.merged == true);
  }
  assert(result.cluster_to_final.at(0) == result.cluster_to_final.at(1));
  assert(result.cluster_to_final.at(1) == result.cluster_to_final.at(2));
}

void test_fragmented_secondary_policy_collapses_to_two_speakers() {
  using svp::audio::sherpa_diarization_internal::collapse_fragmented_secondary_tracks;

  std::vector<svp::audio::SherpaDiarizationSegment> segments;
  segments.push_back(test_diarization_segment(0.0f, 78.0f, 0));
  segments.push_back(test_diarization_segment(100.0f, 11.0f, 1));
  segments.push_back(test_diarization_segment(120.0f, 5.0f, 2));
  segments.push_back(test_diarization_segment(140.0f, 3.0f, 3));
  segments.push_back(test_diarization_segment(160.0f, 1.0f, 4));
  segments.push_back(test_diarization_segment(180.0f, 0.8f, 5));
  segments.push_back(test_diarization_segment(200.0f, 0.6f, 6));
  segments.push_back(test_diarization_segment(220.0f, 0.5f, 7));
  segments.push_back(test_diarization_segment(240.0f, 0.6f, 8));
  segments.push_back(test_diarization_segment(260.0f, 0.5f, 9));

  int32_t speaker_count = 10;
  collapse_fragmented_secondary_tracks(segments, speaker_count, 35);

  assert(speaker_count == 2);
  assert(segments.front().speaker_id == 0);
  for (std::size_t i = 1; i < segments.size(); ++i) {
    assert(segments[i].speaker_id == 1);
  }
}

void test_fragmented_secondary_policy_leaves_flatter_multi_speaker_case() {
  using svp::audio::sherpa_diarization_internal::collapse_fragmented_secondary_tracks;

  std::vector<svp::audio::SherpaDiarizationSegment> segments;
  segments.push_back(test_diarization_segment(0.0f, 63.0f, 0));
  segments.push_back(test_diarization_segment(100.0f, 10.0f, 1));
  segments.push_back(test_diarization_segment(120.0f, 7.0f, 2));
  segments.push_back(test_diarization_segment(140.0f, 5.0f, 3));
  segments.push_back(test_diarization_segment(160.0f, 4.0f, 4));
  segments.push_back(test_diarization_segment(180.0f, 3.0f, 5));
  segments.push_back(test_diarization_segment(200.0f, 2.0f, 6));
  segments.push_back(test_diarization_segment(220.0f, 2.0f, 7));
  segments.push_back(test_diarization_segment(240.0f, 2.0f, 8));
  segments.push_back(test_diarization_segment(260.0f, 2.0f, 9));

  int32_t speaker_count = 10;
  collapse_fragmented_secondary_tracks(segments, speaker_count, 35);

  assert(speaker_count == 10);
  for (std::size_t i = 0; i < segments.size(); ++i) {
    assert(segments[i].speaker_id == static_cast<int32_t>(i));
  }
}

void test_reconcile_clusters_still_works_after_lib_discovery() {
  std::vector<std::vector<float>> sim_matrix = {
      {1.0f, 0.3f},
      {0.3f, 1.0f},
  };
  std::vector<int32_t> cluster_ids = {0, 1};
  svp::audio::ReconciliationResult result =
      svp::audio::reconcile_clusters(sim_matrix, cluster_ids);
  assert(result.final_speaker_count == 2);
  assert(result.cluster_to_final.size() == 2);
  assert(result.cluster_to_final[0] == 0);
  assert(result.cluster_to_final[1] == 1);
}
