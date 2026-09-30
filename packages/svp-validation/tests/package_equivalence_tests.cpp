// Policy tests for `svp-validator validate --equivalent` (RC2 Sections
// 5.16.1, 5.16.2, and 17.5). Each test builds small synthetic packages that
// differ in exactly one governed way and checks the classification.

#include "svp/blocks/block_writer.hpp"
#include "svp/validation/package_equivalence.hpp"

#include "equivalence/equivalence_profile.hpp"
#include "equivalence/mask_metrics.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using svp::validation::EquivalenceClass;
using svp::validation::EquivalenceOutcome;
using svp::validation::EquivalenceReport;
using Profile = svp::validation::equivalence::DefaultEquivalenceProfileV1;

// Fixed ZIP timestamp so two writes of the same entries are byte-identical.
constexpr time_t kFixedZipMtime = 1767225600;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << "\n";
    std::exit(1);
  }
}

struct Entry {
  std::string name;
  std::string content;
  bool stored = false;
};

using Entries = std::vector<Entry>;

fs::path g_root;

fs::path write_zip(const std::string& name, const Entries& entries) {
  const auto path = g_root / name;
  int error = 0;
  zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
  require(archive != nullptr, "zip_open " + path.string());
  for (const auto& entry : entries) {
    zip_source_t* source =
        zip_source_buffer(archive, entry.content.data(), entry.content.size(), 0);
    const auto index = zip_file_add(archive, entry.name.c_str(), source, ZIP_FL_ENC_UTF_8);
    require(index >= 0, "zip_file_add " + entry.name);
    zip_set_file_compression(archive, static_cast<zip_uint64_t>(index),
                             entry.stored ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    zip_file_set_mtime(archive, static_cast<zip_uint64_t>(index), kFixedZipMtime, 0);
  }
  require(zip_close(archive) == 0, "zip_close " + path.string());
  return path;
}

std::string jsonl(const std::vector<nlohmann::json>& records) {
  std::string output;
  for (const auto& record : records) {
    output += record.dump() + "\n";
  }
  return output;
}

nlohmann::json base_manifest() {
  return {{"svp_version", "1.0-rc.2"},
          {"package_id", "svp_equivalence_test_pkg"},
          {"created_utc", "2026-01-01T00:00:00Z"},
          {"canonical_analysis_raster", {{"width", 64}, {"height", 36}}}};
}

std::vector<nlohmann::json> base_words() {
  return {
      {{"id", "word_000000"}, {"start_us", 0}, {"end_us", 400000}, {"text", "hello"},
       {"confidence", 0.9}},
      {{"id", "word_000001"}, {"start_us", 400000}, {"end_us", 800000}, {"text", "world"},
       {"confidence", 0.8}},
  };
}

Entries base_entries(const nlohmann::json& manifest = base_manifest(),
                     const std::vector<nlohmann::json>& words = base_words()) {
  return {
      {"mimetype", "application/vnd.svp+zip", true},
      {"manifest.json", manifest.dump(2) + "\n"},
      {"transcript/words.jsonl", jsonl(words)},
      {"media/original/source_000.bin", std::string("original-media-bytes"), true},
  };
}

EquivalenceReport compare(const fs::path& left,
                          const fs::path& right,
                          bool normalize = false) {
  return svp::validation::compare_packages(
      left, right, svp::validation::EquivalenceOptions{.normalize_build_metadata = normalize});
}

bool has_outcome(const EquivalenceReport& report,
                 EquivalenceOutcome outcome,
                 const std::string& rule_prefix) {
  return std::any_of(report.findings.begin(), report.findings.end(), [&](const auto& finding) {
    return finding.outcome == outcome && finding.rule.rfind(rule_prefix, 0) == 0;
  });
}

void expect_class(const EquivalenceReport& report,
                  EquivalenceClass expected,
                  const std::string& test) {
  if (report.classification != expected) {
    nlohmann::json json = report;
    std::cerr << json.dump(2) << "\n";
  }
  require(report.classification == expected,
          test + ": expected " + std::string{svp::validation::to_string(expected)} + ", got " +
              std::string{svp::validation::to_string(report.classification)});
}

void test_identical_inputs_are_byte_identical() {
  const auto a = write_zip("identical_a.svp", base_entries());
  const auto b = write_zip("identical_b.svp", base_entries());
  expect_class(compare(a, a), EquivalenceClass::byte_identical, "same file");
  expect_class(compare(a, b), EquivalenceClass::byte_identical, "identical copies");
}

void test_reordered_json_keys_are_structurally_equivalent() {
  const auto a = write_zip("keys_a.svp", base_entries());
  auto entries = base_entries();
  // Same values, different key order, indentation, and CRLF line endings.
  entries[1].content =
      "{\r\n\"package_id\":\"svp_equivalence_test_pkg\",\"svp_version\":\"1.0-rc.2\","
      "\"canonical_analysis_raster\":{\"height\":36,\"width\":64},"
      "\"created_utc\":\"2026-01-01T00:00:00Z\"}\r\n";
  entries[2].content =
      "{\"text\":\"hello\",\"id\":\"word_000000\",\"end_us\":400000,\"start_us\":0,"
      "\"confidence\":0.9}\r\n\r\n"
      "{\"confidence\":0.8,\"text\":\"world\",\"start_us\":400000,\"end_us\":800000,"
      "\"id\":\"word_000001\"}\r\n";
  const auto b = write_zip("keys_b.svp", entries);
  const auto report = compare(a, b);
  expect_class(report, EquivalenceClass::structurally_equivalent, "reordered keys");
  require(has_outcome(report, EquivalenceOutcome::canonicalization_only, "canonical_json_jsonl"),
          "reordered keys: canonicalization_only finding");
}

void test_zip_entry_order_is_structurally_equivalent() {
  auto entries = base_entries();
  const auto a = write_zip("order_a.svp", entries);
  std::reverse(entries.begin() + 1, entries.end());
  for (auto& entry : entries) {
    // Compression method is not a content fact either.
    entry.stored = entry.stored || entry.name == "manifest.json";
  }
  const auto b = write_zip("order_b.svp", entries);
  const auto report = compare(a, b);
  expect_class(report, EquivalenceClass::structurally_equivalent, "zip entry order");
  require(report.findings.empty(), "zip entry order: no content findings");
}

void test_confidence_tolerance_policy() {
  const auto a = write_zip("confidence_a.svp", base_entries());

  auto within = base_words();
  within[0]["confidence"] = 0.9 + Profile::kConfidenceAbsMax / 2.0;
  const auto b = write_zip("confidence_within.svp", base_entries(base_manifest(), within));
  const auto report = compare(a, b);
  expect_class(report, EquivalenceClass::numerically_equivalent, "confidence +0.0005");
  require(has_outcome(report, EquivalenceOutcome::within_tolerance, "confidence_fields"),
          "confidence +0.0005: within_tolerance finding");

  auto beyond = base_words();
  beyond[0]["confidence"] = 0.9 + Profile::kConfidenceAbsMax * 2.0;
  const auto c = write_zip("confidence_beyond.svp", base_entries(base_manifest(), beyond));
  const auto beyond_report = compare(a, c);
  expect_class(beyond_report, EquivalenceClass::not_equivalent, "confidence +0.002");
  require(has_outcome(beyond_report, EquivalenceOutcome::not_equivalent, "confidence_fields"),
          "confidence +0.002: rule reported");
}

void test_tolerance_never_applies_to_exact_fields() {
  const auto a = write_zip("exact_a.svp", base_entries());

  auto text = base_words();
  text[1]["text"] = "World";
  expect_class(compare(a, write_zip("exact_text.svp", base_entries(base_manifest(), text))),
               EquivalenceClass::not_equivalent, "text change");

  // A timing change far smaller than any tolerance is still exact-governed.
  auto timing = base_words();
  timing[1]["end_us"] = 800001;
  expect_class(compare(a, write_zip("exact_time.svp", base_entries(base_manifest(), timing))),
               EquivalenceClass::not_equivalent, "time range change");

  auto null_confidence = base_words();
  null_confidence[0]["confidence"] = nullptr;
  expect_class(compare(a, write_zip("exact_null.svp",
                                    base_entries(base_manifest(), null_confidence))),
               EquivalenceClass::not_equivalent, "null matches only null");

  auto extra_record = base_words();
  extra_record.push_back(extra_record.back());
  expect_class(compare(a, write_zip("exact_count.svp",
                                    base_entries(base_manifest(), extra_record))),
               EquivalenceClass::not_equivalent, "record count change");

  auto media = base_entries();
  media[3].content = "original-media-byteZ";
  expect_class(compare(a, write_zip("exact_media.svp", media)),
               EquivalenceClass::not_equivalent, "original media bytes");
}

void test_missing_entry_is_not_equivalent() {
  const auto a = write_zip("missing_a.svp", base_entries());
  auto entries = base_entries();
  entries.pop_back();
  const auto report = compare(a, write_zip("missing_b.svp", entries));
  expect_class(report, EquivalenceClass::not_equivalent, "missing entry");
  require(has_outcome(report, EquivalenceOutcome::not_equivalent, "entry_set"),
          "missing entry: entry_set finding");
}

void test_build_metadata_normalization_is_opt_in() {
  const auto a = write_zip("metadata_a.svp", base_entries());
  auto manifest = base_manifest();
  manifest["created_utc"] = "2026-01-02T03:04:05Z";
  const auto b = write_zip("metadata_b.svp", base_entries(manifest));

  expect_class(compare(a, b), EquivalenceClass::not_equivalent, "metadata without flag");

  const auto normalized = compare(a, b, true);
  expect_class(normalized, EquivalenceClass::structurally_equivalent, "metadata with flag");
  require(has_outcome(normalized, EquivalenceOutcome::normalized_build_metadata,
                      "build_metadata:/created_utc"),
          "metadata with flag: normalized field reported");

  // Normalization covers only registered fields: other changes still count.
  auto words = base_words();
  words[0]["text"] = "jello";
  const auto c = write_zip("metadata_c.svp", base_entries(manifest, words));
  expect_class(compare(a, c, true), EquivalenceClass::not_equivalent,
               "metadata flag does not hide content changes");

  // Absolute host paths in processor input refs are normalized; package-
  // relative refs are not.
  const auto processors = [](const std::string& input) {
    return Entry{"provenance/processors.jsonl",
                 jsonl({{{"id", "proc_ffmpeg_audio_extraction_0001"},
                         {"input_refs", nlohmann::json::array({input})}}})};
  };
  auto host_a = base_entries();
  host_a.push_back(processors("/Users/one/clip.mp4"));
  auto host_b = base_entries();
  host_b.push_back(processors("/Volumes/two/clip.mp4"));
  auto relative_b = base_entries();
  relative_b.push_back(processors("media/original/clip.mp4"));
  const auto pa = write_zip("host_a.svp", host_a);
  expect_class(compare(pa, write_zip("host_b.svp", host_b), true),
               EquivalenceClass::structurally_equivalent, "absolute host path normalized");
  expect_class(compare(pa, write_zip("host_rel.svp", relative_b), true),
               EquivalenceClass::not_equivalent, "relative ref stays exact");
}

void test_audio_and_color_rules() {
  const auto loudness = [](double momentary, const nlohmann::json& shortterm) {
    return Entry{"media/audio/loudness.jsonl",
                 jsonl({{{"id", "loud_0"}, {"start_us", 0}, {"end_us", 400000},
                         {"momentary_lufs", momentary}, {"shortterm_lufs", shortterm},
                         {"true_peak_dbtp", -6.6}}})};
  };
  auto base = base_entries();
  base.push_back(loudness(-19.4, -18.0));
  const auto a = write_zip("loud_a.svp", base);

  auto within = base_entries();
  within.push_back(loudness(-19.4 + Profile::kLoudnessAbsMaxLu / 2.0, -18.0));
  expect_class(compare(a, write_zip("loud_within.svp", within)),
               EquivalenceClass::numerically_equivalent, "loudness within 0.1 LU");

  auto beyond = base_entries();
  beyond.push_back(loudness(-19.4 + Profile::kLoudnessAbsMaxLu * 2.0, -18.0));
  expect_class(compare(a, write_zip("loud_beyond.svp", beyond)),
               EquivalenceClass::not_equivalent, "loudness beyond 0.1 LU");

  auto null_value = base_entries();
  null_value.push_back(loudness(-19.4, nullptr));
  expect_class(compare(a, write_zip("loud_null.svp", null_value)),
               EquivalenceClass::not_equivalent, "loudness null matches only null");

  // The loudness rule is scoped to loudness entries: the same key elsewhere
  // is exact-governed.
  auto words = base_words();
  words[0]["momentary_lufs"] = -19.4;
  auto moved = base_words();
  moved[0]["momentary_lufs"] = -19.4 + Profile::kLoudnessAbsMaxLu / 2.0;
  expect_class(compare(write_zip("scope_a.svp", base_entries(base_manifest(), words)),
                       write_zip("scope_b.svp", base_entries(base_manifest(), moved))),
               EquivalenceClass::not_equivalent, "loudness rule is entry-scoped");

  const auto color = [](double gray, double black, const std::string& dominant) {
    return Entry{"colors/color_observations.jsonl",
                 jsonl({{{"color_observation_id", "color_obs_000001"},
                         {"bucket_coverage", {{"gray", gray}, {"black", black}}},
                         {"coverage_total", gray + black},
                         {"dominant_bucket", dominant}}})};
  };
  auto color_base = base_entries();
  color_base.push_back(color(0.501, 0.499, "gray"));
  const auto ca = write_zip("color_a.svp", color_base);
  auto tied = base_entries();
  tied.push_back(color(0.499, 0.501, "black"));
  expect_class(compare(ca, write_zip("color_tied.svp", tied)),
               EquivalenceClass::numerically_equivalent,
               "dominant bucket may differ when tied within tolerance");
  auto untied = base_entries();
  untied.push_back(color(0.40, 0.60, "black"));
  expect_class(compare(ca, write_zip("color_untied.svp", untied)),
               EquivalenceClass::not_equivalent, "coverage beyond 0.005");
}

void test_geometry_rules() {
  const auto region = [](double x_max) {
    return Entry{"text/text_regions.jsonl",
                 jsonl({{{"text_region_id", "text_region_000001"},
                         {"bbox_norm", {0.1, 0.1, x_max, 0.5}}}})};
  };
  auto base = base_entries();
  base.push_back(region(0.5));
  const auto a = write_zip("bbox_a.svp", base);
  auto within = base_entries();
  within.push_back(region(0.5005));  // IoU 0.99875
  expect_class(compare(a, write_zip("bbox_within.svp", within)),
               EquivalenceClass::numerically_equivalent, "bbox IoU >= 0.995");
  auto beyond = base_entries();
  beyond.push_back(region(0.51));  // IoU 0.9756
  expect_class(compare(a, write_zip("bbox_beyond.svp", beyond)),
               EquivalenceClass::not_equivalent, "bbox IoU < 0.995");

  // Centroids are compared in canonical analysis raster pixels (64x36 here).
  const auto centroid = [](double x) {
    return Entry{"spatial/regions.jsonl",
                 jsonl({{{"id", "region_1"}, {"centroid_norm", {x, 0.5}}}})};
  };
  auto centroid_base = base_entries();
  centroid_base.push_back(centroid(0.5));
  const auto ca = write_zip("centroid_a.svp", centroid_base);
  auto near = base_entries();
  near.push_back(centroid(0.51));  // 0.64 px
  expect_class(compare(ca, write_zip("centroid_near.svp", near)),
               EquivalenceClass::numerically_equivalent, "centroid <= 1 px");
  auto far = base_entries();
  far.push_back(centroid(0.55));  // 3.2 px
  expect_class(compare(ca, write_zip("centroid_far.svp", far)),
               EquivalenceClass::not_equivalent, "centroid > 1 px");
}

std::string block_stream(svp::blocks::BlockType type,
                         svp::blocks::DType dtype,
                         std::uint32_t extent_0,
                         std::uint32_t extent_1,
                         const std::vector<std::byte>& payload) {
  std::vector<std::byte> stream;
  svp::blocks::BlockWriteSpec spec;
  spec.block_type = type;
  spec.dtype = dtype;
  spec.extent_0 = extent_0;
  spec.extent_1 = extent_1;
  spec.extent_2 = 1;
  if (type == svp::blocks::BlockType::embedding) {
    spec.start_frame = std::numeric_limits<std::uint64_t>::max();
    spec.frame_count = 0;
  } else {
    spec.frame_count = 1;
  }
  (void)svp::blocks::write_block(stream, spec, payload.data(), payload.size());
  return {reinterpret_cast<const char*>(stream.data()), stream.size()};
}

template <typename T>
std::vector<std::byte> as_bytes(const std::vector<T>& values) {
  std::vector<std::byte> bytes(values.size() * sizeof(T));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

// Deterministic pseudo-random sequence (incompressible payloads keep the
// compressed size equal when values change, isolating the payload rule).
std::uint32_t next_random(std::uint32_t& state) {
  state = state * 1664525U + 1013904223U;
  return state;
}

std::size_t header_compressed_size(const std::string& stream) {
  constexpr std::size_t kCompressedSizeOffset = 20;  // RC2 Section 14.5 header layout
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(
                 stream[kCompressedSizeOffset + byte]))
             << (8U * byte);
  }
  return static_cast<std::size_t>(value);
}

void test_embedding_block_and_digest_policy() {
  constexpr std::uint32_t kDimension = 64;
  std::uint32_t state = 7;
  std::vector<float> vector(kDimension);
  for (auto& value : vector) {
    value = static_cast<float>(next_random(state) % 2000U) / 1000.0F - 1.0F;
  }
  auto nudged = vector;
  nudged[0] += 0.001F;
  auto rotated = vector;
  std::rotate(rotated.begin(), rotated.begin() + 1, rotated.end());

  const auto entries_for = [&](const std::vector<float>& values, const std::string& name) {
    const auto stream = block_stream(svp::blocks::BlockType::embedding,
                                     svp::blocks::DType::float32, 1, kDimension,
                                     as_bytes(values));
    auto entries = base_entries();
    entries.push_back({"embeddings/embeddings.blocks.svpez", stream, true});
    // The index record carries the block hash; its mismatch is decided by
    // the block payload rule.
    entries.push_back({"embeddings/embeddings.index.jsonl",
                       jsonl({{{"id", "embed_0"},
                               {"block_file", "embeddings/embeddings.blocks.svpez"},
                               {"block_offset", 0},
                               {"compressed_size", header_compressed_size(stream)},
                               {"payload_blake3", name}}})});
    return entries;
  };

  const auto base_stream =
      block_stream(svp::blocks::BlockType::embedding, svp::blocks::DType::float32, 1,
                   kDimension, as_bytes(vector));
  const auto nudged_stream =
      block_stream(svp::blocks::BlockType::embedding, svp::blocks::DType::float32, 1,
                   kDimension, as_bytes(nudged));
  require(header_compressed_size(base_stream) == header_compressed_size(nudged_stream),
          "precondition: equal compressed size");

  const auto a = write_zip("embed_a.svp", entries_for(vector, "hash_a"));
  const auto near = compare(a, write_zip("embed_near.svp", entries_for(nudged, "hash_b")));
  expect_class(near, EquivalenceClass::numerically_equivalent, "embedding cosine >= 0.999");
  require(has_outcome(near,
                      EquivalenceOutcome::hash_mismatch_allowed_by_source_layer_equivalence,
                      "derived_digest:/payload_blake3"),
          "embedding digest mismatch allowed by source layer");

  const auto far = compare(a, write_zip("embed_far.svp", entries_for(rotated, "hash_c")));
  expect_class(far, EquivalenceClass::not_equivalent, "embedding cosine < 0.999");
  require(has_outcome(far, EquivalenceOutcome::not_equivalent, "embeddings.cosine_similarity"),
          "embedding far: cosine rule reported");
  require(has_outcome(far, EquivalenceOutcome::not_equivalent, "derived_digest:/payload_blake3"),
          "digest of a non-equivalent block is not allowed");
}

void test_depth_block_policy() {
  constexpr std::uint32_t kWidth = 16;
  constexpr std::uint32_t kHeight = 16;
  std::uint32_t state = 11;
  std::vector<std::uint16_t> depth(kWidth * kHeight);
  for (auto& value : depth) {
    value = static_cast<std::uint16_t>(next_random(state) >> 16U);
  }
  auto close = depth;
  close[0] = static_cast<std::uint16_t>(close[0] ^ 1U);  // 1/65535 on one pixel
  auto shuffled = depth;
  std::reverse(shuffled.begin(), shuffled.end());

  const auto package = [&](const std::string& name, const std::vector<std::uint16_t>& values) {
    auto entries = base_entries();
    entries.push_back({"spatial/depth.blocks.svpdz",
                       block_stream(svp::blocks::BlockType::depth, svp::blocks::DType::uint16,
                                    kWidth, kHeight, as_bytes(values)),
                       true});
    return write_zip(name, entries);
  };
  const auto a = package("depth_a.svp", depth);
  expect_class(compare(a, package("depth_close.svp", close)),
               EquivalenceClass::numerically_equivalent, "depth within MAE/p99/Spearman");
  const auto far = compare(a, package("depth_far.svp", shuffled));
  expect_class(far, EquivalenceClass::not_equivalent, "depth beyond MAE/p99/Spearman");
  require(has_outcome(far, EquivalenceOutcome::not_equivalent, "depth_maps."),
          "depth far: decoded depth rule reported");
}

std::vector<std::byte> rle(const std::vector<std::uint8_t>& pixels) {
  std::vector<std::uint64_t> runs;
  std::uint8_t current = 0;
  std::uint64_t run = 0;
  for (const auto pixel : pixels) {
    if (pixel != current) {
      runs.push_back(run);
      run = 0;
      current = pixel;
    }
    ++run;
  }
  runs.push_back(run);
  std::vector<std::byte> bytes;
  for (auto value : runs) {
    do {
      auto byte = static_cast<std::uint8_t>(value & 0x7FU);
      value >>= 7U;
      if (value != 0) {
        byte |= 0x80U;
      }
      bytes.push_back(static_cast<std::byte>(byte));
    } while (value != 0);
  }
  return bytes;
}

void test_mask_block_policy() {
  constexpr std::uint32_t kSize = 32;
  const auto square = [&](std::uint32_t origin, bool drop_corner) {
    std::vector<std::uint8_t> pixels(kSize * kSize, 0);
    for (std::uint32_t y = origin; y < origin + 20; ++y) {
      for (std::uint32_t x = origin; x < origin + 20; ++x) {
        pixels[y * kSize + x] = 1;
      }
    }
    if (drop_corner) {
      pixels[(origin + 19) * kSize + origin + 19] = 0;
    }
    return pixels;
  };
  const auto package = [&](const std::string& name, const std::vector<std::uint8_t>& pixels) {
    auto entries = base_entries();
    entries.push_back({"spatial/masks.blocks.svpmz",
                       block_stream(svp::blocks::BlockType::mask,
                                    svp::blocks::DType::svp_rle_v1, kSize, kSize, rle(pixels)),
                       true});
    return write_zip(name, entries);
  };
  // Mask geometry rule on decoded planes: one dropped corner pixel of a
  // 20x20 square keeps IoU = 399/400 and boundary displacement <= 1 px.
  const auto plane = [&](const std::vector<std::uint8_t>& pixels) {
    auto planes = svp::validation::equivalence::decode_mask_planes(
        rle(pixels), static_cast<std::uint32_t>(svp::blocks::DType::svp_rle_v1), kSize, kSize, 1);
    require(planes.has_value() && planes->size() == 1, "mask RLE decodes");
    return planes->front();
  };
  const auto close = svp::validation::equivalence::mask_metrics(plane(square(4, false)),
                                                                plane(square(4, true)));
  require(close.iou >= Profile::kMaskIouMin, "mask close: IoU within profile");
  require(close.boundary_displacement_px <= Profile::kMaskBoundaryDisplacementPxMax,
          "mask close: boundary displacement within profile");
  const auto shifted = svp::validation::equivalence::mask_metrics(plane(square(4, false)),
                                                                  plane(square(5, false)));
  require(shifted.iou < Profile::kMaskIouMin, "mask shifted: IoU below profile");

  // Block level: Section 17.5 keeps compressed_size exact, so a payload
  // change that alters the compressed size is not equivalent regardless of
  // the decoded geometry.
  const auto a = package("mask_a.svp", square(4, false));
  const auto report = compare(a, package("mask_far.svp", square(5, false)));
  expect_class(report, EquivalenceClass::not_equivalent, "mask block change");
  expect_class(compare(a, package("mask_same.svp", square(4, false))),
               EquivalenceClass::byte_identical, "mask block unchanged");
}

std::string sqlite_index(double confidence, const std::string& created_utc) {
  const auto path = g_root / "index.tmp.sqlite";
  fs::remove(path);
  sqlite3* database = nullptr;
  require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK, "sqlite open");
  const auto sql =
      "CREATE TABLE svp_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
      "CREATE TABLE relationships (relationship_id TEXT PRIMARY KEY, source_id TEXT NOT NULL,"
      " confidence REAL NOT NULL);"
      "INSERT INTO svp_meta VALUES ('created_utc', '" +
      created_utc +
      "');"
      "INSERT INTO relationships VALUES ('rel_1', 'word_000000', " +
      std::to_string(confidence) + ");";
  require(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
          "sqlite exec");
  sqlite3_close(database);
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_sqlite_logical_policy() {
  const auto package = [&](const std::string& name, double confidence,
                           const std::string& created_utc) {
    auto entries = base_entries();
    entries.push_back({"index/index.sqlite", sqlite_index(confidence, created_utc), true});
    return write_zip(name, entries);
  };
  const std::string created = "2026-01-01T00:00:00Z";
  const auto a = package("sqlite_a.svp", 0.9, created);
  expect_class(compare(a, package("sqlite_within.svp", 0.9004, created)),
               EquivalenceClass::numerically_equivalent, "sqlite confidence within 0.001");
  expect_class(compare(a, package("sqlite_beyond.svp", 0.95, created)),
               EquivalenceClass::not_equivalent, "sqlite confidence beyond 0.001");

  const auto later = package("sqlite_later.svp", 0.9, "2026-01-02T00:00:00Z");
  expect_class(compare(a, later), EquivalenceClass::not_equivalent,
               "sqlite svp_meta created_utc without flag");
  expect_class(compare(a, later, true), EquivalenceClass::structurally_equivalent,
               "sqlite svp_meta created_utc with flag");
}

void test_ineligible_inputs() {
  const auto a = write_zip("kind_a.svp", base_entries());
  const auto b = write_zip("kind_b.svpi", base_entries());
  expect_class(compare(a, b), EquivalenceClass::not_equivalent, ".svp vs .svpi");
  expect_class(compare(b, write_zip("kind_c.svpi", base_entries())),
               EquivalenceClass::byte_identical, ".svpi sidecars are comparable");
  expect_class(compare(a, g_root / "absent.svp"), EquivalenceClass::not_equivalent,
               "missing input");
}

double registry_number(const nlohmann::json& rules, const char* layer, const char* field) {
  return rules.at(layer).at(field).get<double>();
}

void test_profile_matches_spec_registry(const fs::path& registry_path) {
  std::ifstream input{registry_path};
  require(static_cast<bool>(input), "open " + registry_path.string());
  const auto registry = nlohmann::json::parse(input);
  const auto& rules = registry.at("rules");
  require(registry_number(rules, "depth_maps", "mean_absolute_error_max") ==
              Profile::kDepthMeanAbsErrorMax,
          "depth MAE");
  require(registry_number(rules, "depth_maps", "p99_absolute_error_max") ==
              Profile::kDepthP99AbsErrorMax,
          "depth p99");
  require(registry_number(rules, "depth_maps", "spearman_rank_correlation_min") ==
              Profile::kDepthSpearmanMin,
          "depth spearman");
  require(registry_number(rules, "embeddings", "cosine_similarity_min") ==
              Profile::kEmbeddingCosineMin,
          "embedding cosine");
  require(registry_number(rules, "masks", "iou_min") == Profile::kMaskIouMin, "mask IoU");
  require(registry_number(rules, "masks", "p95_boundary_displacement_px_max") ==
              Profile::kMaskBoundaryDisplacementPxMax,
          "mask boundary");
  require(registry_number(rules, "entity_tracks", "bbox_iou_min") ==
              Profile::kBoundingBoxIouMin,
          "bbox IoU");
  require(registry_number(rules, "entity_tracks", "centroid_displacement_px_max") ==
              Profile::kCentroidDisplacementPxMax,
          "centroid");
  require(registry_number(rules, "confidence_fields", "absolute_difference_max") ==
              Profile::kConfidenceAbsMax,
          "confidence");

  std::map<std::string, nlohmann::json> layers;
  for (const auto& layer : registry.at("layer_rules")) {
    layers[layer.at("layer").get<std::string>()] = layer;
  }
  require(layers.at("text_regions").at("bbox_iou_min").get<double>() ==
              Profile::kBoundingBoxIouMin,
          "text region bbox");
  require(layers.at("color_observations").at("bucket_abs_tolerance").get<double>() ==
              Profile::kColorBucketCoverageAbsMax,
          "color bucket");
  require(layers.at("color_observations").at("coverage_total_tolerance").get<double>() ==
              Profile::kColorCoverageTotalAbsMax,
          "color total");
  require(layers.at("loudness").at("loudness_abs_tolerance_lu").get<double>() ==
              Profile::kLoudnessAbsMaxLu,
          "loudness");
  require(layers.at("loudness").at("true_peak_abs_tolerance_dbtp").get<double>() ==
              Profile::kTruePeakAbsMaxDbtp,
          "true peak");
  require(layers.at("spectrum").at("band_abs_tolerance_db").get<double>() ==
              Profile::kSpectrumAbsMaxDb,
          "spectrum");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: svp-package-equivalence-tests <equivalence-profile.json>\n";
    return 2;
  }
  g_root = fs::temp_directory_path() /
           ("svp-equivalence-tests-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(g_root);

  test_profile_matches_spec_registry(argv[1]);
  test_identical_inputs_are_byte_identical();
  test_reordered_json_keys_are_structurally_equivalent();
  test_zip_entry_order_is_structurally_equivalent();
  test_confidence_tolerance_policy();
  test_tolerance_never_applies_to_exact_fields();
  test_missing_entry_is_not_equivalent();
  test_build_metadata_normalization_is_opt_in();
  test_audio_and_color_rules();
  test_geometry_rules();
  test_embedding_block_and_digest_policy();
  test_depth_block_policy();
  test_mask_block_policy();
  test_sqlite_logical_policy();
  test_ineligible_inputs();

  fs::remove_all(g_root);
  std::cout << "svp-package-equivalence-tests passed\n";
  return 0;
}
