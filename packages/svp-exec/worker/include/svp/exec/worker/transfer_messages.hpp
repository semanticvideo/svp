#pragma once

// Runtime and blob transfer messages (plan §4.3). Every byte that moves is
// content-addressed: a blob is named by its BLAKE3 and length, travels in
// BLOB_PUT chunks (each chunk a frame payload, which the frame decoder
// verifies against the BLAKE3 its header declares), and is verified whole
// before it enters the worker's CAS. Runtimes and model bundles are then
// assembled on the worker from blobs it already holds, so no message ever
// carries a path, a command, or an environment variable (plan §4.3 safety).
//
// Puts are not answered; the coordinator confirms with the matching query
// afterwards. A put the worker cannot accept ends the session with ERROR.
//
// RUNTIME_HAVE  query  {"runtime_id":"b3:<hex>"}
//               answer {"present":bool,"runtime_id":"b3:<hex>"}
// RUNTIME_PUT          {"components":{BlobRef},   optional: components.json
//                       "manifest":{BlobRef},     manifest.json
//                       "runtime_id":"b3:<hex>"}
// BLOB_HAVE     query  {"blobs":[BlobRef...],"kind":"blobs"}
//                      {"kind":"model_bundles","model_bundles":["<hex>"...]}
//               answer {"kind":"blobs","missing":[BlobRef...]}
//                      {"kind":"model_bundles","missing":["<hex>"...]}
// BLOB_PUT      chunk  {"blake3","bytes","kind":"chunk","offset"} + 1 payload
//               bundle {"kind":"model_bundle","lock":{model-lock with exactly
//                       one entry},"manifest":{BlobRef}}
//
// BlobRef = {"blake3":"<hex>","bytes":n}. All bodies are strict: unknown
// members are rejected.

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/frame.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace svp::exec::worker {

struct BlobRef {
  Blake3Digest blake3{};
  std::uint64_t bytes = 0;

  bool operator==(const BlobRef&) const = default;
  auto operator<=>(const BlobRef&) const = default;
};

[[nodiscard]] BlobRef blob_ref_for(std::span<const std::byte> bytes);

// RUNTIME_HAVE
[[nodiscard]] Frame make_runtime_query_frame(const Blake3Digest& runtime_id);
[[nodiscard]] Blake3Digest runtime_query_from_frame(const Frame& frame);

struct RuntimeAnswer {
  Blake3Digest runtime_id{};
  bool present = false;

  bool operator==(const RuntimeAnswer&) const = default;
};
[[nodiscard]] Frame make_runtime_answer_frame(const RuntimeAnswer& answer);
[[nodiscard]] RuntimeAnswer runtime_answer_from_frame(const Frame& frame);

// RUNTIME_PUT
struct RuntimePut {
  Blake3Digest runtime_id{};
  BlobRef manifest;
  std::optional<BlobRef> components;

  bool operator==(const RuntimePut&) const = default;
};
[[nodiscard]] Frame make_runtime_put_frame(const RuntimePut& put);
[[nodiscard]] RuntimePut runtime_put_from_frame(const Frame& frame);

// BLOB_HAVE
struct BlobQuery {
  std::vector<BlobRef> blobs;
  bool operator==(const BlobQuery&) const = default;
};
struct ModelBundleQuery {
  std::vector<Blake3Digest> bundles;
  bool operator==(const ModelBundleQuery&) const = default;
};
using BlobHaveQuery = std::variant<BlobQuery, ModelBundleQuery>;

[[nodiscard]] Frame make_blob_query_frame(const BlobQuery& query);
[[nodiscard]] Frame make_model_bundle_query_frame(const ModelBundleQuery& query);
[[nodiscard]] BlobHaveQuery blob_have_query_from_frame(const Frame& frame);

// The answer lists what is missing, in query order.
[[nodiscard]] Frame make_blob_answer_frame(const BlobQuery& missing);
[[nodiscard]] Frame make_model_bundle_answer_frame(const ModelBundleQuery& missing);
[[nodiscard]] BlobQuery blob_answer_from_frame(const Frame& frame);
[[nodiscard]] ModelBundleQuery model_bundle_answer_from_frame(const Frame& frame);

// BLOB_PUT
struct BlobChunk {
  BlobRef blob;
  std::uint64_t offset = 0;
  bool operator==(const BlobChunk&) const = default;
};
struct ModelBundlePut {
  // A model-lock.json document with exactly one entry: the bundle's lock
  // record from the coordinator's lock (svp-model-lock-1).
  nlohmann::json lock = nlohmann::json::object();
  BlobRef manifest;
  bool operator==(const ModelBundlePut&) const = default;
};
using BlobPut = std::variant<BlobChunk, ModelBundlePut>;

// `chunk` becomes the frame's one payload. Throws ExecError(invalid_value)
// when the chunk is empty or does not fit inside the blob at `offset`.
[[nodiscard]] Frame make_blob_chunk_frame(const BlobChunk& header,
                                          std::vector<std::byte> chunk);
[[nodiscard]] Frame make_model_bundle_put_frame(const ModelBundlePut& put);
// For a chunk, the payload is frame.payloads[0] (checked to fit the blob).
[[nodiscard]] BlobPut blob_put_from_frame(const Frame& frame);

}  // namespace svp::exec::worker
