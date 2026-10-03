#include "svp/exec/worker/transfer_messages.hpp"

#include "message_fields.hpp"

#include <utility>

namespace svp::exec::worker {
namespace {

using namespace detail;

constexpr std::string_view kKindBlobs = "blobs";
constexpr std::string_view kKindModelBundles = "model_bundles";
constexpr std::string_view kKindChunk = "chunk";
constexpr std::string_view kKindModelBundle = "model_bundle";

nlohmann::json blob_ref_to_json(const BlobRef& blob) {
  return nlohmann::json{{"blake3", blake3_hex(blob.blake3)}, {"bytes", blob.bytes}};
}

BlobRef blob_ref_from_json(const nlohmann::json& value, std::string_view path) {
  require_object(value, path);
  reject_unknown_fields(value, {"blake3", "bytes"}, path);
  return BlobRef{.blake3 = required_blake3_hex(value, "blake3", path),
                 .bytes = required_unsigned(value, "bytes", path)};
}

nlohmann::json blob_refs_to_json(const std::vector<BlobRef>& blobs) {
  nlohmann::json array = nlohmann::json::array();
  for (const BlobRef& blob : blobs) {
    array.push_back(blob_ref_to_json(blob));
  }
  return array;
}

std::vector<BlobRef> blob_refs_from_json(const nlohmann::json& body, std::string_view name,
                                         std::string_view path) {
  const nlohmann::json& array = required_array(body, name, path);
  std::vector<BlobRef> blobs;
  blobs.reserve(array.size());
  const std::string item_path = child_path(path, name);
  for (const nlohmann::json& item : array) {
    blobs.push_back(blob_ref_from_json(item, item_path));
  }
  return blobs;
}

std::string required_kind(const nlohmann::json& body, std::string_view path) {
  return required_string(body, "kind", path);
}

[[noreturn]] void unknown_kind(std::string_view path, const std::string& kind) {
  throw ExecError(ExecErrorCode::invalid_value,
                  child_path(path, "kind") + " is not a known kind: `" + kind + "`");
}

}  // namespace

BlobRef blob_ref_for(std::span<const std::byte> bytes) {
  return BlobRef{.blake3 = blake3_digest(bytes), .bytes = bytes.size()};
}

Frame make_runtime_query_frame(const Blake3Digest& runtime_id) {
  return Frame{.type = MessageType::runtime_have,
               .body = nlohmann::json{{"runtime_id", blake3_prefixed(runtime_id)}},
               .payloads = {}};
}

Blake3Digest runtime_query_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::runtime_have, 0);
  reject_unknown_fields(frame.body, {"runtime_id"}, path);
  return required_blake3_prefixed(frame.body, "runtime_id", path);
}

Frame make_runtime_answer_frame(const RuntimeAnswer& answer) {
  return Frame{.type = MessageType::runtime_have,
               .body = nlohmann::json{{"present", answer.present},
                                      {"runtime_id", blake3_prefixed(answer.runtime_id)}},
               .payloads = {}};
}

RuntimeAnswer runtime_answer_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::runtime_have, 0);
  reject_unknown_fields(frame.body, {"present", "runtime_id"}, path);
  return RuntimeAnswer{.runtime_id = required_blake3_prefixed(frame.body, "runtime_id", path),
                       .present = required_bool(frame.body, "present", path)};
}

Frame make_runtime_put_frame(const RuntimePut& put) {
  nlohmann::json body{{"manifest", blob_ref_to_json(put.manifest)},
                      {"runtime_id", blake3_prefixed(put.runtime_id)}};
  if (put.components) {
    body["components"] = blob_ref_to_json(*put.components);
  }
  return Frame{.type = MessageType::runtime_put, .body = std::move(body), .payloads = {}};
}

RuntimePut runtime_put_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::runtime_put, 0);
  reject_unknown_fields(frame.body, {"components", "manifest", "runtime_id"}, path);
  RuntimePut put;
  put.runtime_id = required_blake3_prefixed(frame.body, "runtime_id", path);
  put.manifest = blob_ref_from_json(required_field(frame.body, "manifest", path),
                                    child_path(path, "manifest"));
  if (frame.body.contains("components")) {
    put.components = blob_ref_from_json(frame.body.at("components"),
                                        child_path(path, "components"));
  }
  return put;
}

Frame make_blob_query_frame(const BlobQuery& query) {
  return Frame{.type = MessageType::blob_have,
               .body = nlohmann::json{{"blobs", blob_refs_to_json(query.blobs)},
                                      {"kind", std::string(kKindBlobs)}},
               .payloads = {}};
}

Frame make_model_bundle_query_frame(const ModelBundleQuery& query) {
  return Frame{.type = MessageType::blob_have,
               .body = nlohmann::json{{"kind", std::string(kKindModelBundles)},
                                      {"model_bundles", digests_to_json(query.bundles, false)}},
               .payloads = {}};
}

BlobHaveQuery blob_have_query_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::blob_have, 0);
  const std::string kind = required_kind(frame.body, path);
  if (kind == kKindBlobs) {
    reject_unknown_fields(frame.body, {"blobs", "kind"}, path);
    return BlobQuery{.blobs = blob_refs_from_json(frame.body, "blobs", path)};
  }
  if (kind == kKindModelBundles) {
    reject_unknown_fields(frame.body, {"kind", "model_bundles"}, path);
    return ModelBundleQuery{
        .bundles = required_digest_array(frame.body, "model_bundles", path, false)};
  }
  unknown_kind(path, kind);
}

Frame make_blob_answer_frame(const BlobQuery& missing) {
  return Frame{.type = MessageType::blob_have,
               .body = nlohmann::json{{"kind", std::string(kKindBlobs)},
                                      {"missing", blob_refs_to_json(missing.blobs)}},
               .payloads = {}};
}

Frame make_model_bundle_answer_frame(const ModelBundleQuery& missing) {
  return Frame{.type = MessageType::blob_have,
               .body = nlohmann::json{{"kind", std::string(kKindModelBundles)},
                                      {"missing", digests_to_json(missing.bundles, false)}},
               .payloads = {}};
}

BlobQuery blob_answer_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::blob_have, 0);
  reject_unknown_fields(frame.body, {"kind", "missing"}, path);
  if (const std::string kind = required_kind(frame.body, path); kind != kKindBlobs) {
    unknown_kind(path, kind);
  }
  return BlobQuery{.blobs = blob_refs_from_json(frame.body, "missing", path)};
}

ModelBundleQuery model_bundle_answer_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::blob_have, 0);
  reject_unknown_fields(frame.body, {"kind", "missing"}, path);
  if (const std::string kind = required_kind(frame.body, path); kind != kKindModelBundles) {
    unknown_kind(path, kind);
  }
  return ModelBundleQuery{.bundles = required_digest_array(frame.body, "missing", path, false)};
}

Frame make_blob_chunk_frame(const BlobChunk& header, std::vector<std::byte> chunk) {
  if (chunk.empty() || header.offset > header.blob.bytes ||
      chunk.size() > header.blob.bytes - header.offset) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "BLOB_PUT chunk must be non-empty and lie inside its blob");
  }
  Frame frame{.type = MessageType::blob_put,
              .body = nlohmann::json{{"blake3", blake3_hex(header.blob.blake3)},
                                     {"bytes", header.blob.bytes},
                                     {"kind", std::string(kKindChunk)},
                                     {"offset", header.offset}},
              .payloads = {}};
  frame.payloads.push_back(std::move(chunk));
  return frame;
}

Frame make_model_bundle_put_frame(const ModelBundlePut& put) {
  return Frame{.type = MessageType::blob_put,
               .body = nlohmann::json{{"kind", std::string(kKindModelBundle)},
                                      {"lock", put.lock},
                                      {"manifest", blob_ref_to_json(put.manifest)}},
               .payloads = {}};
}

BlobPut blob_put_from_frame(const Frame& frame) {
  if (frame.type != MessageType::blob_put) {
    (void)require_frame(frame, MessageType::blob_put, frame.payloads.size());
  }
  const std::string path = "BLOB_PUT.body";
  require_object(frame.body, path);
  const std::string kind = required_kind(frame.body, path);
  if (kind == kKindChunk) {
    (void)require_frame(frame, MessageType::blob_put, 1);
    reject_unknown_fields(frame.body, {"blake3", "bytes", "kind", "offset"}, path);
    BlobChunk chunk{.blob = BlobRef{.blake3 = required_blake3_hex(frame.body, "blake3", path),
                                    .bytes = required_unsigned(frame.body, "bytes", path)},
                    .offset = required_unsigned(frame.body, "offset", path)};
    const std::size_t size = frame.payloads.front().size();
    if (size == 0 || chunk.offset > chunk.blob.bytes ||
        size > chunk.blob.bytes - chunk.offset) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "BLOB_PUT chunk must be non-empty and lie inside its blob");
    }
    return chunk;
  }
  if (kind == kKindModelBundle) {
    (void)require_frame(frame, MessageType::blob_put, 0);
    reject_unknown_fields(frame.body, {"kind", "lock", "manifest"}, path);
    return ModelBundlePut{.lock = required_object(frame.body, "lock", path),
                          .manifest = blob_ref_from_json(required_field(frame.body, "manifest", path),
                                                         child_path(path, "manifest"))};
  }
  unknown_kind(path, kind);
}

Frame make_blob_get_frame(const BlobRef& blob) {
  return Frame{.type = MessageType::blob_get,
               .body = nlohmann::json{{"blob", blob_ref_to_json(blob)}},
               .payloads = {}};
}

BlobRef blob_get_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::blob_get, 0);
  reject_unknown_fields(frame.body, {"blob"}, path);
  return blob_ref_from_json(required_field(frame.body, "blob", path), child_path(path, "blob"));
}

Frame make_blob_release_frame(const std::vector<BlobRef>& blobs) {
  return Frame{.type = MessageType::blob_release,
               .body = nlohmann::json{{"blobs", blob_refs_to_json(blobs)}},
               .payloads = {}};
}

std::vector<BlobRef> blob_release_from_frame(const Frame& frame) {
  const std::string path = require_frame(frame, MessageType::blob_release, 0);
  reject_unknown_fields(frame.body, {"blobs"}, path);
  return blob_refs_from_json(frame.body, "blobs", path);
}

}  // namespace svp::exec::worker
