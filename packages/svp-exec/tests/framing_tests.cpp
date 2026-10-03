#include "exec_test_support.hpp"
#include "svp/exec/frame.hpp"
#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/message_type.hpp"
#include "svp/exec/task_frames.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

// Canonical header of hello_frame(); 159 bytes. The payload digest is the
// official BLAKE3 vector for "abc".
constexpr std::string_view kHelloHeader =
    R"({"body":{"protocol":{"major":1,"minor":0}},)"
    R"("payloads":[{"blake3":"6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85","bytes":3}],)"
    R"("type":"HELLO"})";

Frame hello_frame() {
  return Frame{.type = MessageType::hello,
               .body = nlohmann::json{{"protocol", {{"major", 1}, {"minor", 0}}}},
               .payloads = {to_bytes("abc")}};
}

std::vector<std::byte> length_prefix(std::uint32_t value) {
  return {static_cast<std::byte>(value >> 24U), static_cast<std::byte>(value >> 16U),
          static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)};
}

std::vector<std::byte> raw_frame(std::string_view header, std::string_view payloads) {
  std::vector<std::byte> bytes = length_prefix(static_cast<std::uint32_t>(header.size()));
  const std::vector<std::byte> header_bytes = to_bytes(header);
  const std::vector<std::byte> payload_bytes = to_bytes(payloads);
  bytes.insert(bytes.end(), header_bytes.begin(), header_bytes.end());
  bytes.insert(bytes.end(), payload_bytes.begin(), payload_bytes.end());
  return bytes;
}

void test_message_type_names() {
  expect(kAllMessageTypes.size() == 23,
         "plan §4.3 defines 17 message types; worker protocol 1.1 adds BLOB_GET, 1.2 "
         "BLOB_RELEASE, and the fleet join listener JOIN_OFFER, JOIN_CHALLENGE, JOIN_ACCEPT, "
         "and JOIN_DONE");
  for (const MessageType type : kAllMessageTypes) {
    expect(parse_message_type(message_type_name(type)) == type,
           "message type name round trip");
  }
  expect_equal(message_type_name(MessageType::hello_ack), "HELLO_ACK", "HELLO_ACK");
  expect_equal(message_type_name(MessageType::runtime_have), "RUNTIME_HAVE",
               "RUNTIME_HAVE");
  expect(!parse_message_type("hello"), "lower-case type accepted");
  expect(!parse_message_type("BLOB_HAVE?"), "query spelling accepted");
}

void test_golden_frame_bytes() {
  expect(kHelloHeader.size() == 159, "golden header length");
  const std::vector<std::byte> encoded = encode_frame(hello_frame());
  expect(encoded == raw_frame(kHelloHeader, "abc"), "golden frame bytes");
  expect(decode_frame(encoded) == hello_frame(), "golden frame decodes");
}

void test_round_trips() {
  const std::vector<Frame> frames = {
      Frame{.type = MessageType::heartbeat, .body = nlohmann::json::object(), .payloads = {}},
      hello_frame(),
      Frame{.type = MessageType::blob_put,
            .body = nlohmann::json{{"chunk", 0}},
            .payloads = {to_bytes(""), to_bytes(std::string(5000, 'x')),
                         to_bytes("tail")}},
  };
  std::vector<std::byte> stream;
  for (const Frame& frame : frames) {
    expect(decode_frame(encode_frame(frame)) == frame, "single frame round trip");
    const std::vector<std::byte> bytes = encode_frame(frame);
    stream.insert(stream.end(), bytes.begin(), bytes.end());
  }

  // All frames in one feed.
  FrameDecoder bulk;
  bulk.feed(stream);
  for (const Frame& frame : frames) {
    expect(bulk.next() == frame, "bulk decode order");
  }
  expect(!bulk.next(), "no extra frames");
  bulk.finish();

  // One byte at a time: frames appear exactly when their last byte arrives.
  FrameDecoder trickle;
  std::vector<Frame> decoded;
  for (const std::byte byte : stream) {
    trickle.feed(std::span(&byte, 1));
    while (auto frame = trickle.next()) {
      decoded.push_back(std::move(*frame));
    }
  }
  expect(decoded == frames, "byte-at-a-time decode");
  expect(trickle.buffered_bytes() == 0, "nothing left buffered");
  trickle.finish();
}

void test_oversize_header_rejected() {
  // The decoder rejects from the length prefix alone, before the header
  // arrives.
  FrameDecoder decoder;
  decoder.feed(length_prefix(kDefaultMaxFrameHeaderBytes + 1));
  expect_exec_error(ExecErrorCode::frame_header_too_large,
                    [&] { static_cast<void>(decoder.next()); },
                    "header length above the default limit");
  expect_exec_error(ExecErrorCode::frame_malformed,
                    [&] { static_cast<void>(decoder.next()); },
                    "decoder stays failed after an error");

  const FrameLimits tight{.max_header_bytes = 100};
  expect_exec_error(ExecErrorCode::frame_header_too_large,
                    [&] { static_cast<void>(encode_frame(hello_frame(), tight)); },
                    "encoder refuses an oversize header");
  expect_exec_error(ExecErrorCode::frame_header_too_large,
                    [&] {
                      static_cast<void>(decode_frame(encode_frame(hello_frame()), tight));
                    },
                    "decoder enforces a tightened header limit");
}

void test_oversize_payload_rejected() {
  // Declared sizes are checked from the header, before payload bytes arrive.
  const std::string over_default =
      R"({"body":{},"payloads":[{"blake3":")" + std::string(64, '0') +
      R"(","bytes":)" + std::to_string(kDefaultMaxFramePayloadBytes + 1) +
      R"(}],"type":"BLOB_PUT"})";
  FrameDecoder decoder;
  decoder.feed(raw_frame(over_default, ""));
  expect_exec_error(ExecErrorCode::frame_payload_too_large,
                    [&] { static_cast<void>(decoder.next()); },
                    "payload above the default limit");

  const FrameLimits tight{.max_payload_bytes = 4, .max_total_payload_bytes = 6};
  const Frame big{.type = MessageType::blob_put,
                  .body = nlohmann::json::object(),
                  .payloads = {to_bytes("12345")}};
  expect_exec_error(ExecErrorCode::frame_payload_too_large,
                    [&] { static_cast<void>(encode_frame(big, tight)); },
                    "encoder refuses an oversize payload");
  expect_exec_error(ExecErrorCode::frame_payload_too_large,
                    [&] { static_cast<void>(decode_frame(encode_frame(big), tight)); },
                    "decoder enforces a tightened payload limit");
  const Frame many{.type = MessageType::blob_put,
                   .body = nlohmann::json::object(),
                   .payloads = {to_bytes("1234"), to_bytes("123")}};
  expect_exec_error(ExecErrorCode::frame_payload_too_large,
                    [&] { static_cast<void>(decode_frame(encode_frame(many), tight)); },
                    "decoder enforces the per-frame payload total");
}

void test_truncated_frames_rejected() {
  const std::vector<std::byte> encoded = encode_frame(hello_frame());
  for (const std::size_t keep : {std::size_t{2}, std::size_t{4}, std::size_t{100},
                                 encoded.size() - 1}) {
    const std::vector<std::byte> partial(encoded.begin(),
                                         encoded.begin() + static_cast<std::ptrdiff_t>(keep));
    FrameDecoder decoder;
    decoder.feed(partial);
    expect(!decoder.next(), "partial frame is not released");
    expect_exec_error(ExecErrorCode::frame_truncated, [&] { decoder.finish(); },
                      "stream ending inside a frame");
    expect_exec_error(ExecErrorCode::frame_truncated,
                      [&] { static_cast<void>(decode_frame(partial)); },
                      "decode_frame of a partial buffer");
  }
  std::vector<std::byte> trailing = encoded;
  trailing.push_back(std::byte{0});
  expect_exec_error(ExecErrorCode::frame_malformed,
                    [&] { static_cast<void>(decode_frame(trailing)); },
                    "bytes after a single frame");
}

void test_payload_hash_mismatch_rejected() {
  std::vector<std::byte> encoded = encode_frame(hello_frame());
  encoded.back() = static_cast<std::byte>('d');
  expect_exec_error(ExecErrorCode::payload_hash_mismatch,
                    [&] { static_cast<void>(decode_frame(encoded)); },
                    "payload that does not match its declared BLAKE3");
}

void test_malformed_headers_rejected() {
  expect_exec_error(ExecErrorCode::frame_malformed,
                    [] { static_cast<void>(decode_frame(length_prefix(0))); },
                    "zero-length header");
  expect_exec_error(
      ExecErrorCode::unknown_message_type,
      [] {
        static_cast<void>(decode_frame(
            raw_frame(R"({"body":{},"payloads":[],"type":"PING"})", "")));
      },
      "unknown message type");
  expect_exec_error(
      ExecErrorCode::non_canonical_json,
      [] {
        static_cast<void>(decode_frame(
            raw_frame(R"({"type":"HELLO","body":{},"payloads":[]})", "")));
      },
      "header with unsorted keys");
  expect_exec_error(
      ExecErrorCode::unknown_field,
      [] {
        static_cast<void>(decode_frame(raw_frame(
            R"({"body":{},"payloads":[],"type":"HELLO","version":1})", "")));
      },
      "header with an unknown field");
  expect_exec_error(
      ExecErrorCode::missing_field,
      [] {
        static_cast<void>(
            decode_frame(raw_frame(R"({"body":{},"type":"HELLO"})", "")));
      },
      "header without payload declarations");
  expect_exec_error(
      ExecErrorCode::wrong_type,
      [] {
        static_cast<void>(decode_frame(
            raw_frame(R"({"body":[],"payloads":[],"type":"HELLO"})", "")));
      },
      "array body");
  expect_exec_error(
      ExecErrorCode::invalid_digest,
      [] {
        static_cast<void>(decode_frame(raw_frame(
            R"({"body":{},"payloads":[{"blake3":"abc","bytes":0}],"type":"HELLO"})",
            "")));
      },
      "short payload digest");
}

void test_task_records_travel_in_frames() {
  const TaskSpec spec = sample_task_spec();
  const Lease lease{.lease_id = "lease_1",
                    .attempt = 1,
                    .duration = std::chrono::milliseconds(30'000),
                    .heartbeat_interval = std::chrono::milliseconds(5'000)};
  const Frame assign = decode_frame(encode_frame(make_leased_assign_frame(spec, lease)));
  expect(assign.type == MessageType::assign, "ASSIGN type");
  expect(leased_assignment_from_frame(assign).spec == spec, "TaskSpec through ASSIGN");

  const std::vector<std::byte> output = to_bytes("HELLO WORLD");
  TaskResult result = sample_task_result();
  result.outputs = {make_artifact_ref(output, "text/plain", "upper_text")};
  result.output_digest = compute_output_digest(result.outputs);
  const Frame frame = decode_frame(encode_frame(make_result_frame(result, {output})));
  expect(task_result_from_result_frame(frame) == result, "TaskResult through RESULT");
  expect(frame.payloads.front() == output, "RESULT payload bytes");

  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] {
                      static_cast<void>(make_result_frame(result, {to_bytes("hello world")}));
                    },
                    "RESULT built with the wrong payload");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(make_result_frame(result, {})); },
                    "RESULT built without its payload");

  Frame forged = frame;
  forged.payloads.front() = to_bytes("HELLO_WORLD");
  expect_exec_error(ExecErrorCode::payload_hash_mismatch,
                    [&] { static_cast<void>(task_result_from_result_frame(forged)); },
                    "RESULT payload that does not match its output ref");
  expect_exec_error(ExecErrorCode::frame_malformed,
                    [&] { static_cast<void>(leased_assignment_from_frame(frame)); },
                    "RESULT read as ASSIGN");
  // ASSIGN always carries its lease (plan §4.3); a bare TaskSpec body is not
  // an ASSIGN.
  Frame lease_less = assign;
  lease_less.body.erase("lease");
  expect_exec_error(ExecErrorCode::missing_field,
                    [&] { static_cast<void>(leased_assignment_from_frame(lease_less)); },
                    "ASSIGN without a lease");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec framing tests",
      {{"message_type_names", test_message_type_names},
       {"golden_frame_bytes", test_golden_frame_bytes},
       {"round_trips", test_round_trips},
       {"oversize_header_rejected", test_oversize_header_rejected},
       {"oversize_payload_rejected", test_oversize_payload_rejected},
       {"truncated_frames_rejected", test_truncated_frames_rejected},
       {"payload_hash_mismatch_rejected", test_payload_hash_mismatch_rejected},
       {"malformed_headers_rejected", test_malformed_headers_rejected},
       {"task_records_travel_in_frames", test_task_records_travel_in_frames}});
}
