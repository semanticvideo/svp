#include "svp/exec/worker/coordinator_session.hpp"

#include "svp/models/hash.hpp"

#include "svp/exec/lease_frames.hpp"
#include "svp/exec/worker/transfer_messages.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace svp::exec::worker {

CoordinatorHello make_coordinator_hello(const CoordinatorRuntime& runtime,
                                        const svp::models::ThreadPlan& thread_plan,
                                        std::optional<ModelSetSummary> model_set) {
  CoordinatorHello hello;
  hello.runtime_id = runtime.runtime_id;
  hello.runtime_kind = runtime.kind;
  hello.host = detect_host_facts();
  hello.thread_plan = svp::models::thread_plan_to_json(thread_plan);
  hello.thread_plan_host_independent = svp::models::thread_plan_is_host_independent(thread_plan);
  hello.model_set = std::move(model_set);
  return hello;
}

WorkerSessionClient::WorkerSessionClient(FrameReader& reader, FrameWriter& writer,
                                         FrameLimits limits)
    : reader_(reader), writer_(writer), limits_(limits) {}

Frame WorkerSessionClient::expect(MessageType type) {
  std::optional<Frame> frame = reader_.read();
  if (!frame) {
    throw WorkerError(WorkerErrorCode::protocol,
                      "the worker closed the session while " +
                          std::string(message_type_name(type)) + " was expected");
  }
  if (frame->type == MessageType::error) {
    const auto message = frame->body.find("message");
    const auto code = frame->body.find("code");
    throw WorkerError(
        WorkerErrorCode::protocol,
        "worker reported an error" +
            (code != frame->body.end() && code->is_string() ? " (" + code->get<std::string>() + ")"
                                                           : std::string()) +
            ": " +
            (message != frame->body.end() && message->is_string() ? message->get<std::string>()
                                                                  : frame->body.dump()));
  }
  if (frame->type != type) {
    throw WorkerError(WorkerErrorCode::protocol,
                      "expected " + std::string(message_type_name(type)) + " from the worker, got " +
                          std::string(message_type_name(frame->type)));
  }
  return std::move(*frame);
}

WorkerHelloAck WorkerSessionClient::hello(const CoordinatorHello& hello) {
  writer_.write(make_hello_frame(hello));
  WorkerHelloAck ack = hello_ack_from_frame(expect(MessageType::hello_ack));
  if (!ack.accepted()) {
    throw WorkerError(WorkerErrorCode::refused,
                      "worker refused the session (" +
                          std::string(session_refusal_code_name(ack.refusal->code)) +
                          "): " + ack.refusal->message);
  }
  if (!protocol_compatible(kWorkerProtocolVersion, ack.protocol)) {
    throw WorkerError(WorkerErrorCode::refused, "worker speaks protocol major " +
                                                    std::to_string(ack.protocol.major));
  }
  return ack;
}

bool WorkerSessionClient::runtime_present(const Blake3Digest& runtime_id) {
  writer_.write(make_runtime_query_frame(runtime_id));
  const RuntimeAnswer answer = runtime_answer_from_frame(expect(MessageType::runtime_have));
  if (answer.runtime_id != runtime_id) {
    throw WorkerError(WorkerErrorCode::protocol, "RUNTIME_HAVE answered for another runtime");
  }
  return answer.present;
}

void WorkerSessionClient::send_blobs(const std::vector<BlobSource>& blobs, TransferStats& stats) {
  std::vector<BlobSource> unique;
  std::set<BlobRef> seen;
  for (const BlobSource& blob : blobs) {
    if (seen.insert(blob.ref).second) {
      unique.push_back(blob);
    }
  }
  BlobQuery query;
  for (const BlobSource& blob : unique) {
    query.blobs.push_back(blob.ref);
  }
  writer_.write(make_blob_query_frame(query));
  const BlobQuery missing = blob_answer_from_frame(expect(MessageType::blob_have));
  const std::set<BlobRef> wanted(missing.blobs.begin(), missing.blobs.end());
  stats.blobs_already_present += unique.size() - wanted.size();
  const std::uint64_t chunk_limit = limits_.max_payload_bytes;
  for (const BlobSource& blob : unique) {
    if (!wanted.contains(blob.ref)) {
      continue;
    }
    std::ifstream file(blob.file, std::ios::binary);
    if (!file) {
      throw WorkerError(WorkerErrorCode::io, "cannot read " + blob.file.string());
    }
    std::uint64_t offset = 0;
    while (offset < blob.ref.bytes) {
      const std::uint64_t size = std::min(chunk_limit, blob.ref.bytes - offset);
      std::vector<std::byte> chunk(size);
      file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(size));
      if (static_cast<std::uint64_t>(file.gcount()) != size) {
        throw WorkerError(WorkerErrorCode::io,
                          blob.file.string() + " changed while it was being sent");
      }
      writer_.write(make_blob_chunk_frame(BlobChunk{.blob = blob.ref, .offset = offset},
                                          std::move(chunk)));
      offset += size;
      stats.bytes_sent += size;
    }
    ++stats.blobs_sent;
  }
  if (!wanted.empty()) {
    writer_.write(make_blob_query_frame(query));
    const BlobQuery still_missing = blob_answer_from_frame(expect(MessageType::blob_have));
    if (!still_missing.blobs.empty()) {
      throw WorkerError(WorkerErrorCode::verification,
                        "worker still lacks blob " + blake3_hex(still_missing.blobs.front().blake3) +
                            " after it was sent");
    }
  }
}

void WorkerSessionClient::fetch_blob(const BlobRef& blob,
                                     const std::filesystem::path& destination) {
  writer_.write(make_blob_get_frame(blob));
  if (!blob_answer_from_frame(expect(MessageType::blob_have)).blobs.empty()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "the worker does not hold blob " + blake3_hex(blob.blake3));
  }
  const std::filesystem::path partial = destination.string() + ".partial";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw WorkerError(WorkerErrorCode::io, "cannot write " + partial.string());
    }
    std::uint64_t received = 0;
    while (received < blob.bytes) {
      const Frame frame = expect(MessageType::blob_put);
      const BlobPut put = blob_put_from_frame(frame);
      const BlobChunk* chunk = std::get_if<BlobChunk>(&put);
      if (chunk == nullptr || chunk->blob != blob || chunk->offset != received) {
        throw WorkerError(WorkerErrorCode::protocol,
                          "the worker sent blob " + blake3_hex(blob.blake3) + " out of order");
      }
      const FramePayload& bytes = frame.payloads.front();
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      if (!out) {
        throw WorkerError(WorkerErrorCode::io, "cannot write " + partial.string());
      }
      received += bytes.size();
    }
  }
  std::error_code error;
  if (std::filesystem::file_size(partial, error) != blob.bytes || error ||
      svp::models::blake3_hex_for_file(partial) != blake3_hex(blob.blake3)) {
    std::filesystem::remove(partial, error);
    throw WorkerError(WorkerErrorCode::verification,
                      "blob " + blake3_hex(blob.blake3) + " from the worker does not verify");
  }
  std::filesystem::rename(partial, destination, error);
  if (error) {
    std::filesystem::remove(partial, error);
    throw WorkerError(WorkerErrorCode::io, "cannot move the fetched blob to " +
                                               destination.string());
  }
}

void WorkerSessionClient::ensure_runtime(const CoordinatorRuntime& runtime, TransferStats& stats) {
  if (runtime_present(runtime.runtime_id)) {
    return;
  }
  std::vector<BlobSource> blobs;
  for (const RuntimeFileSource& file : runtime.files) {
    blobs.push_back(file.blob);
  }
  send_blobs(blobs, stats);

  // The manifest and components.json travel as blobs like every file (one
  // chunk each); the worker reads them back from its CAS.
  const auto send_bytes = [&](std::string_view bytes) {
    const std::vector<std::byte> data(reinterpret_cast<const std::byte*>(bytes.data()),
                                      reinterpret_cast<const std::byte*>(bytes.data()) +
                                          bytes.size());
    const BlobRef ref = blob_ref_for(data);
    writer_.write(make_blob_query_frame(BlobQuery{.blobs = {ref}}));
    if (!blob_answer_from_frame(expect(MessageType::blob_have)).blobs.empty()) {
      writer_.write(make_blob_chunk_frame(BlobChunk{.blob = ref, .offset = 0}, data));
      ++stats.blobs_sent;
      stats.bytes_sent += data.size();
    }
    return ref;
  };
  RuntimePut put{.runtime_id = runtime.runtime_id,
                 .manifest = send_bytes(runtime.manifest_bytes),
                 .components = std::nullopt};
  if (runtime.components_bytes) {
    put.components = send_bytes(*runtime.components_bytes);
  }
  writer_.write(make_runtime_put_frame(put));
  if (!runtime_present(runtime.runtime_id)) {
    throw WorkerError(WorkerErrorCode::verification,
                      "worker did not install runtime " + blake3_prefixed(runtime.runtime_id));
  }
  stats.runtime_pushed = true;
}

std::vector<Blake3Digest> WorkerSessionClient::missing_model_bundles(
    const std::vector<Blake3Digest>& bundles) {
  writer_.write(make_model_bundle_query_frame(ModelBundleQuery{.bundles = bundles}));
  return model_bundle_answer_from_frame(expect(MessageType::blob_have)).bundles;
}

void WorkerSessionClient::ensure_model_bundles(const std::vector<ModelBundleSource>& bundles,
                                               TransferStats& stats) {
  std::vector<Blake3Digest> digests;
  for (const ModelBundleSource& bundle : bundles) {
    digests.push_back(bundle.bundle_blake3);
  }
  const std::vector<Blake3Digest> missing = missing_model_bundles(digests);
  for (const ModelBundleSource& bundle : bundles) {
    if (std::find(missing.begin(), missing.end(), bundle.bundle_blake3) == missing.end()) {
      continue;
    }
    std::vector<BlobSource> blobs = bundle.files;
    blobs.push_back(bundle.manifest);
    send_blobs(blobs, stats);
    writer_.write(make_model_bundle_put_frame(
        ModelBundlePut{.lock = bundle.lock, .manifest = bundle.manifest.ref}));
    if (!missing_model_bundles({bundle.bundle_blake3}).empty()) {
      throw WorkerError(WorkerErrorCode::verification,
                        "worker did not install model bundle " + bundle.model_bundle_id);
    }
    stats.model_bundles_pushed.push_back(bundle.model_bundle_id);
  }
}

void WorkerSessionClient::shutdown() { writer_.write(make_shutdown_frame()); }

}  // namespace svp::exec::worker
