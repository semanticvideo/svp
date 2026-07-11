#include "private.hpp"
#include "chunk_embedding_pass.hpp"
#include "fragmented_speaker_fallback.hpp"
#include "speaker_count_estimator.hpp"

#include "svp/core/memory_diagnostics.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace svp::audio {

using namespace sherpa_diarization_internal;
namespace {

struct ChunkSpeakerEvidence {
  int32_t local_speaker = -1;
  float first_start_sec = 0.0f;
  float last_end_sec = 0.0f;
  std::vector<float> embedding;
};

bool segments_overlap(
    const std::vector<SherpaDiarizationSegment>& left,
    const std::vector<SherpaDiarizationSegment>& right) {
  for (const auto& left_segment : left) {
    for (const auto& right_segment : right) {
      if (left_segment.start_sec < right_segment.end_sec &&
          right_segment.start_sec < left_segment.end_sec) {
        return true;
      }
    }
  }
  return false;
}

std::size_t chunk_count_for_sample_count(std::size_t sample_count) {
  std::size_t total_chunks = 0;
  for (const auto& window : build_diarization_windows(sample_count)) {
    const std::size_t window_samples =
        window.process_end - window.process_start;
    total_chunks +=
        (window_samples +
         static_cast<std::size_t>(kMaxDiarizationChunkSamples) - 1) /
        static_cast<std::size_t>(kMaxDiarizationChunkSamples);
  }
  return total_chunks;
}

}  // namespace

std::size_t diarization_chunk_count(
    const std::filesystem::path& wav_path) {
  const PcmS16MonoWavInfo wav_info = read_pcm_s16le_mono_wav_info(wav_path);
  return chunk_count_for_sample_count(wav_info.sample_count);
}

SherpaDiarizationResult run_sherpa_diarization(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::vector<AsrWord>& words,
    DiarizationProgressCallback on_progress) {
  SherpaDiarizationResult result;

  const SherpaDiarizationApi& api = get_api();
  if (!api.lib_handle || !api.create) {
    std::string blocker = "sherpa-onnx C API library not loaded. Attempted paths:";
    std::vector<std::string> attempted = sherpa_lib_paths_attempted();
    if (attempted.empty()) {
      blocker += " (none — library discovery was not triggered)";
    } else {
      for (std::size_t i = 0; i < attempted.size(); ++i) {
        blocker += "\n  [" + std::to_string(i + 1) + "] " + attempted[i];
      }
    }
    result.blockers.push_back(blocker);
    return result;
  }

  const std::filesystem::path segmentation_model =
      model_dir / "sherpa-onnx-pyannote-segmentation-3-0" / "model.onnx";
  const std::filesystem::path embedding_model =
      model_dir / "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";

  if (!std::filesystem::exists(segmentation_model)) {
    result.blockers.push_back("segmentation model not found: " + segmentation_model.string());
    return result;
  }
  if (!std::filesystem::exists(embedding_model)) {
    result.blockers.push_back("embedding model not found: " + embedding_model.string());
    return result;
  }

  PcmS16MonoWavInfo wav_info;
  try {
    wav_info = read_pcm_s16le_mono_wav_info(wav_path);
  } catch (const std::exception& e) {
    result.blockers.push_back(std::string("failed to read WAV: ") + e.what());
    return result;
  }

  if (wav_info.sample_count == 0) {
    result.blockers.push_back("WAV has no audio samples");
    return result;
  }

  const std::string seg_path = segmentation_model.string();
  const std::string emb_path = embedding_model.string();

  SherpaOnnxOfflineSpeakerDiarizationConfig config;
  std::memset(&config, 0, sizeof(config));
  config.segmentation.pyannote.model = seg_path.c_str();
  config.segmentation.num_threads = 1;
  config.segmentation.debug = 0;
  config.segmentation.provider = "cpu";
  config.embedding.model = emb_path.c_str();
  config.embedding.num_threads = 1;
  config.embedding.debug = 0;
  config.embedding.provider = "cpu";
  config.clustering.num_clusters = -1;
  config.clustering.threshold = kSherpaLocalClusteringThreshold;
  config.min_duration_on = 0.3f;
  config.min_duration_off = 0.5f;

  bool embedding_api_available = (api.emb_create && api.emb_destroy && api.emb_dim &&
                                   api.emb_create_stream && api.stream_accept &&
                                   api.stream_input_finished && api.emb_is_ready &&
                                   api.emb_compute && api.emb_destroy_vec && api.stream_destroy);

  SherpaOnnxSpeakerEmbeddingExtractorConfig emb_config;
  std::memset(&emb_config, 0, sizeof(emb_config));
  emb_config.model = emb_path.c_str();
  emb_config.num_threads = 1;
  emb_config.debug = 0;
  emb_config.provider = "cpu";

  if (!embedding_api_available) {
    result.blockers.push_back(
        "sherpa-onnx embedding extractor C API not available; using window-local speakers");
  }

  const void* extractor = nullptr;
  int32_t embedding_dim = 0;
  if (embedding_api_available) {
    extractor = api.emb_create(&emb_config);
    if (extractor) {
      embedding_dim = api.emb_dim(extractor);
    } else {
      result.blockers.push_back(
          "sherpa-onnx failed to create speaker embedding extractor; using window-local speakers");
    }
  }
  auto destroy_extractor = [&]() {
    if (extractor) {
      api.emb_destroy(extractor);
      extractor = nullptr;
    }
  };

  std::vector<SherpaDiarizationSegment> preliminary_segments;
  std::vector<SherpaDiarizationSegment> raw_chunk_segments;
  std::vector<SpeakerObservation> speaker_observations;
  std::set<std::pair<int32_t, int32_t>> cannot_link_observations;
  std::vector<SpeakerObservation> raw_chunk_speaker_observations;
  std::set<std::pair<int32_t, int32_t>> raw_chunk_cannot_link_observations;
  int32_t preliminary_speakers = 0;

  const auto windows = build_diarization_windows(wav_info.sample_count);
  const std::size_t total_chunks =
      chunk_count_for_sample_count(wav_info.sample_count);
  std::size_t completed_chunks = 0;
  if (on_progress && total_chunks > 0) {
    on_progress(0, total_chunks);
  }
  const auto complete_progress = [&]() {
    if (on_progress && completed_chunks < total_chunks) {
      on_progress(total_chunks, total_chunks);
    }
  };
  for (std::size_t wi = 0; wi < windows.size(); ++wi) {
    const auto& win = windows[wi];
    const float accepted_start_sec =
        static_cast<float>(win.accepted_start) /
        static_cast<float>(kDiarizationSampleRate);
    const float accepted_end_sec =
        static_cast<float>(win.accepted_end) /
        static_cast<float>(kDiarizationSampleRate);

    svp::core::trace_memory_event("diarization.window.start", {
        {"window_index", std::to_string(wi)},
        {"window_count", std::to_string(windows.size())},
        {"accepted_start", std::to_string(win.accepted_start)},
        {"accepted_end", std::to_string(win.accepted_end)},
        {"process_start", std::to_string(win.process_start)},
        {"process_end", std::to_string(win.process_end)},
        {"speaker_observation_count", std::to_string(speaker_observations.size())}
    });

    const void* sd = api.create(&config);
    if (!sd) {
      result.blockers.push_back(
          "sherpa-onnx failed to create diarization pipeline "
          "(config validation failed) at window " + std::to_string(wi));
      destroy_extractor();
      return result;
    }

    std::vector<float> window_samples;
    try {
      window_samples =
          read_pcm_s16le_mono_wav_range(wav_info, win.process_start, win.process_end);
    } catch (const std::exception& e) {
        result.blockers.push_back(
            std::string("failed to read WAV window: ") + e.what());
      api.destroy(sd);
      destroy_extractor();
      return result;
    }
    if (window_samples.empty()) {
      api.destroy(sd);
      continue;
    }

    int32_t window_speaker_groups = 0;
    std::map<int32_t, LocalSpeakerAssignment> window_local_to_global;
    std::set<int32_t> traced_observation_ids;
    for (std::size_t sample_offset = 0;
         sample_offset < window_samples.size();
         sample_offset += static_cast<std::size_t>(kMaxDiarizationChunkSamples)) {
      const std::size_t remaining = window_samples.size() - sample_offset;
      const int32_t chunk_samples = static_cast<int32_t>(
          std::min<std::size_t>(remaining,
              static_cast<std::size_t>(kMaxDiarizationChunkSamples)));

      const void* diar_result =
          api.process(sd, window_samples.data() + sample_offset, chunk_samples);
      if (!diar_result) {
        result.blockers.push_back(
            "sherpa-onnx diarization process returned null at window " +
            std::to_string(wi));
        api.destroy(sd);
        destroy_extractor();
        return result;
      }

      std::map<int32_t, std::vector<SherpaDiarizationSegment>> chunk_speakers;
      const int32_t num_segments = api.get_num_segments(diar_result);
      const float chunk_start_sec =
          static_cast<float>(win.process_start + sample_offset) /
          static_cast<float>(kDiarizationSampleRate);

      const SherpaOnnxOfflineSpeakerDiarizationSegment* seg_array =
          reinterpret_cast<const SherpaOnnxOfflineSpeakerDiarizationSegment*>(
              api.sort_by_start_time(diar_result));

      for (int32_t i = 0; i < num_segments; ++i) {
        SherpaDiarizationSegment seg;
        seg.start_sec = chunk_start_sec + seg_array[i].start;
        seg.end_sec = chunk_start_sec + seg_array[i].end;
        seg.speaker_id = seg_array[i].speaker;

        if (clip_segment_to_range(seg, accepted_start_sec,
                                  accepted_end_sec)) {
          chunk_speakers[seg.speaker_id].push_back(seg);
        }
      }

      api.destroy_segment(seg_array);
      api.destroy_result(diar_result);

      std::vector<ChunkSpeakerEvidence> chunk_speaker_evidence;
      chunk_speaker_evidence.reserve(chunk_speakers.size());
      for (const auto& [local_speaker, segments] : chunk_speakers) {
        const float first_start_sec = segments.front().start_sec;
        const float last_end_sec = segments.back().end_sec;
        float speech_sec = 0.0f;
        for (const auto& seg : segments) {
          speech_sec += std::max(0.0f, seg.end_sec - seg.start_sec);
        }
        chunk_speaker_evidence.push_back(
            {local_speaker, first_start_sec, last_end_sec,
             {}});
      }

      std::vector<int32_t> chunk_observation_ids;
      chunk_observation_ids.reserve(chunk_speaker_evidence.size());
      for (std::size_t evidence_index = 0;
           evidence_index < chunk_speaker_evidence.size(); ++evidence_index) {
        auto& evidence = chunk_speaker_evidence[evidence_index];
        int32_t global_speaker = -1;
        auto known = window_local_to_global.find(evidence.local_speaker);
        if (known != window_local_to_global.end() &&
            evidence.first_start_sec - known->second.last_end_sec <=
                kLocalSpeakerAssignmentMaxGapSec) {
          global_speaker = known->second.global_speaker;
          known->second.last_end_sec =
              std::max(known->second.last_end_sec, evidence.last_end_sec);
        } else {
          global_speaker = static_cast<int32_t>(speaker_observations.size());
          if (extractor && embedding_dim > 0) {
            const auto& segments =
                std::next(chunk_speakers.begin(), evidence_index)->second;
            evidence.embedding = compute_bounded_speaker_embedding(
                api, extractor, embedding_dim, window_samples,
                win.process_start, segments);
          }
          speaker_observations.push_back(
              {global_speaker, evidence.first_start_sec,
               std::move(evidence.embedding)});
          window_local_to_global[evidence.local_speaker] =
              {global_speaker, evidence.last_end_sec};
          ++window_speaker_groups;
        }
        chunk_observation_ids.push_back(global_speaker);
      }
      std::vector<int32_t> raw_chunk_observation_ids;
      raw_chunk_observation_ids.reserve(chunk_speaker_evidence.size());
      for (const auto& evidence : chunk_speaker_evidence) {
        const int32_t observation_id =
            static_cast<int32_t>(raw_chunk_speaker_observations.size());
        raw_chunk_speaker_observations.push_back(
            {observation_id, evidence.first_start_sec, {}});
        raw_chunk_observation_ids.push_back(observation_id);
      }
      for (std::size_t left = 0;
           left < raw_chunk_observation_ids.size(); ++left) {
        for (std::size_t right = left + 1;
             right < raw_chunk_observation_ids.size(); ++right) {
          const auto& left_segments =
              std::next(chunk_speakers.begin(), left)->second;
          const auto& right_segments =
              std::next(chunk_speakers.begin(), right)->second;
          if (segments_overlap(left_segments, right_segments)) {
            raw_chunk_cannot_link_observations.insert(
                std::minmax(raw_chunk_observation_ids[left],
                            raw_chunk_observation_ids[right]));
          }
        }
      }
      std::size_t evidence_index = 0;
      for (const auto& [local_speaker, segments] : chunk_speakers) {
        const int32_t global_speaker =
            chunk_observation_ids[evidence_index];
        const auto& evidence = chunk_speaker_evidence[evidence_index];
        float speech_sec = 0.0f;
        for (const auto& segment : segments) {
          speech_sec += std::max(0.0f, segment.end_sec - segment.start_sec);
        }
        if (traced_observation_ids.insert(global_speaker).second) {
          svp::core::trace_memory_event("diarization.speaker_observation.new", {
              {"window_index", std::to_string(wi)},
              {"local_speaker", std::to_string(local_speaker)},
              {"observation_id", std::to_string(global_speaker)},
              {"first_start_sec", std::to_string(evidence.first_start_sec)},
              {"last_end_sec", std::to_string(evidence.last_end_sec)},
              {"speech_sec", std::to_string(speech_sec)},
              {"embedding_signal",
               has_embedding_signal(speaker_observations[
                   static_cast<std::size_t>(global_speaker)].embedding)
                   ? "true" : "false"}
          });
        }

        for (SherpaDiarizationSegment seg : segments) {
          SherpaDiarizationSegment raw_segment = seg;
          raw_segment.speaker_id =
              raw_chunk_observation_ids[evidence_index];
          raw_chunk_segments.push_back(raw_segment);
          seg.speaker_id = global_speaker;
          preliminary_segments.push_back(seg);
        }

        svp::core::check_memory_limit("diarization.window.speaker", {
            {"window_index", std::to_string(wi)},
            {"local_speaker", std::to_string(local_speaker)},
            {"global_speaker", std::to_string(global_speaker)}
        });
        ++evidence_index;
      }
      for (std::size_t i = 0; i < chunk_observation_ids.size(); ++i) {
        for (std::size_t j = i + 1; j < chunk_observation_ids.size(); ++j) {
          cannot_link_observations.insert(
              std::minmax(chunk_observation_ids[i], chunk_observation_ids[j]));
        }
      }
      ++completed_chunks;
      if (on_progress) {
        on_progress(completed_chunks, total_chunks);
      }
    }
    api.destroy(sd);

    svp::core::check_memory_limit("diarization.window.process_done", {
        {"window_index", std::to_string(wi)},
        {"window_speakers", std::to_string(window_speaker_groups)}
    });

    preliminary_speakers += window_speaker_groups;

    svp::core::trace_memory_event("diarization.window.end", {
        {"window_index", std::to_string(wi)},
        {"window_speakers", std::to_string(window_speaker_groups)},
        {"speaker_observation_count", std::to_string(speaker_observations.size())},
        {"preliminary_segments", std::to_string(preliminary_segments.size())}
    });
  }

  std::sort(preliminary_segments.begin(), preliminary_segments.end(),
            [](const SherpaDiarizationSegment& a,
               const SherpaDiarizationSegment& b) {
              if (a.start_sec != b.start_sec) return a.start_sec < b.start_sec;
              if (a.end_sec != b.end_sec) return a.end_sec < b.end_sec;
              return a.speaker_id < b.speaker_id;
            });
  std::sort(raw_chunk_segments.begin(), raw_chunk_segments.end(),
            [](const SherpaDiarizationSegment& a,
               const SherpaDiarizationSegment& b) {
              if (a.start_sec != b.start_sec) return a.start_sec < b.start_sec;
              if (a.end_sec != b.end_sec) return a.end_sec < b.end_sec;
              return a.speaker_id < b.speaker_id;
            });

  result.preliminary_cluster_count = preliminary_speakers;
  result.preliminary_segments = preliminary_segments;

  if (preliminary_segments.empty()) {
    result.ran = true;
    result.segments.clear();
    result.final_speaker_count = 0;
    result.reconciliation_method =
        "windowed_sherpa_5min_overlap_2s; no_speech_detected";
    destroy_extractor();
    complete_progress();
    return result;
  }

  const std::map<int32_t, int32_t> observation_to_final =
      cluster_speaker_observations(speaker_observations,
                                   cannot_link_observations);
  result.segments.reserve(preliminary_segments.size());
  for (SherpaDiarizationSegment seg : preliminary_segments) {
    auto mapped = observation_to_final.find(seg.speaker_id);
    if (mapped != observation_to_final.end()) {
      seg.speaker_id = mapped->second;
    }
    result.segments.push_back(seg);
  }
  std::set<int32_t> final_speakers;
  for (const auto& [observation_id, final_speaker] : observation_to_final) {
    (void)observation_id;
    final_speakers.insert(final_speaker);
  }
  result.final_speaker_count =
      static_cast<int32_t>(final_speakers.empty()
                               ? speaker_observations.size()
                               : final_speakers.size());
  stitch_dominant_non_overlapping_tracks(
      result.segments, result.final_speaker_count, speaker_observations.size());
  const auto current_speaker_embeddings = build_reconciled_speaker_embeddings(
      speaker_observations, preliminary_segments, result.segments,
      result.final_speaker_count);
  collapse_fragmented_secondary_tracks(
      result.segments, result.final_speaker_count, speaker_observations.size(),
      current_speaker_embeddings);
  collapse_single_dominant_track(result.segments, result.final_speaker_count);

  SpeakerCountEstimate spectral_count;
  bool used_fragmented_spectral_fallback = false;
  if (extractor && embedding_dim > 0 &&
      has_fragmented_speaker_shape(
          result.segments, result.final_speaker_count,
          speaker_observations.size())) {
    populate_chunk_speaker_embeddings(
        api, extractor, embedding_dim, wav_info,
        raw_chunk_speaker_observations, raw_chunk_segments);
    spectral_count = estimate_speaker_count(
        raw_chunk_speaker_observations,
        raw_chunk_cannot_link_observations);
    svp::core::trace_memory_event("diarization.spectral_count_estimate", {
        {"observation_count",
         std::to_string(raw_chunk_speaker_observations.size())},
        {"speaker_count", std::to_string(spectral_count.speaker_count)},
        {"selected_eigengap",
         std::to_string(spectral_count.selected_eigengap)},
        {"assignments_respect_cannot_link",
         spectral_count.assignments_respect_cannot_link ? "true" : "false"}
    });
  }
  if (should_use_fragmented_speaker_fallback(
          result.segments, result.final_speaker_count,
          speaker_observations.size(), spectral_count)) {
    result.segments.clear();
    result.segments.reserve(raw_chunk_segments.size());
    for (SherpaDiarizationSegment segment : raw_chunk_segments) {
      const auto mapped =
          spectral_count.observation_to_speaker.find(segment.speaker_id);
      if (mapped != spectral_count.observation_to_speaker.end()) {
        segment.speaker_id = mapped->second;
      }
      result.segments.push_back(segment);
    }
    preliminary_segments = raw_chunk_segments;
    speaker_observations = raw_chunk_speaker_observations;
    result.final_speaker_count =
        static_cast<int32_t>(spectral_count.speaker_count);
    used_fragmented_spectral_fallback = true;
    svp::core::trace_memory_event("diarization.fragmented_spectral_fallback", {
        {"final_speaker_count",
         std::to_string(result.final_speaker_count)},
        {"observation_count", std::to_string(speaker_observations.size())}
    });
  }

  result.final_speaker_fingerprints.assign(
      static_cast<std::size_t>(std::max(0, result.final_speaker_count)),
      std::vector<float>{});
  std::vector<int32_t> fingerprint_counts(
      static_cast<std::size_t>(std::max(0, result.final_speaker_count)), 0);
  for (std::size_t i = 0;
       i < preliminary_segments.size() && i < result.segments.size();
       ++i) {
    const int32_t observation_id = preliminary_segments[i].speaker_id;
    const int32_t final_speaker = result.segments[i].speaker_id;
    if (observation_id < 0 ||
        observation_id >= static_cast<int32_t>(speaker_observations.size()) ||
        final_speaker < 0 ||
        final_speaker >= result.final_speaker_count) {
      continue;
    }
    const std::vector<float>& embedding =
        speaker_observations[static_cast<std::size_t>(observation_id)].embedding;
    if (!has_embedding_signal(embedding)) continue;

    std::vector<float>& prototype =
        result.final_speaker_fingerprints[static_cast<std::size_t>(final_speaker)];
    if (prototype.empty()) {
      prototype.assign(embedding.size(), 0.0f);
    }
    if (prototype.size() != embedding.size()) continue;
    for (std::size_t dim = 0; dim < embedding.size(); ++dim) {
      prototype[dim] += embedding[dim];
    }
    ++fingerprint_counts[static_cast<std::size_t>(final_speaker)];
  }
  for (std::size_t speaker = 0;
       speaker < result.final_speaker_fingerprints.size();
       ++speaker) {
    std::vector<float>& prototype = result.final_speaker_fingerprints[speaker];
    const int32_t count = fingerprint_counts[speaker];
    if (count <= 0 || prototype.empty()) continue;
    for (float& value : prototype) {
      value /= static_cast<float>(count);
    }
    normalize_embedding(prototype);
  }
  result.segment_fingerprint_similarities.assign(
      result.segments.size(),
      std::vector<float>(
          static_cast<std::size_t>(std::max(0, result.final_speaker_count)), -2.0f));
  for (std::size_t i = 0;
       i < preliminary_segments.size() && i < result.segments.size();
       ++i) {
    const int32_t observation_id = preliminary_segments[i].speaker_id;
    if (observation_id < 0 ||
        observation_id >= static_cast<int32_t>(speaker_observations.size())) {
      continue;
    }
    const std::vector<float>& embedding =
        speaker_observations[static_cast<std::size_t>(observation_id)].embedding;
    if (!has_embedding_signal(embedding)) continue;
    for (int32_t speaker = 0; speaker < result.final_speaker_count; ++speaker) {
      const std::vector<float>& prototype =
          result.final_speaker_fingerprints[static_cast<std::size_t>(speaker)];
      if (!has_embedding_signal(prototype)) continue;
      result.segment_fingerprint_similarities[i][static_cast<std::size_t>(speaker)] =
          cosine_similarity(embedding, prototype);
    }
  }

  if (extractor && embedding_dim > 0 && !words.empty()) {
    result.word_speaker_assignments =
        assign_word_speakers_with_extractor(
            api, extractor, embedding_dim, wav_info, words, result);
  }
  destroy_extractor();

  result.reconciliation_method =
      "windowed_sherpa_5min_5s_feed_overlap_2s; "
      "bounded_10s_speaker_observations_global_gap_or_floor_reconciliation; "
      "dominant_and_fragmented_secondary_track_policy";
  if (used_fragmented_spectral_fallback) {
    result.reconciliation_method +=
        "; gated_chunk_spectral_fragment_reconciliation";
  }

  result.ran = true;
  complete_progress();
  return result;
}


}  // namespace svp::audio
