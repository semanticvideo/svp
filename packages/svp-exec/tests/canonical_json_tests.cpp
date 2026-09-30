#include "exec_test_support.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/canonical_json.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

void test_blake3_matches_reference_vectors() {
  // Official BLAKE3 vectors (empty input and "abc").
  expect_equal(blake3_hex(blake3_digest(std::string_view())),
               "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
               "BLAKE3 of the empty input");
  expect_equal(blake3_hex(blake3_digest(std::string_view("abc"))),
               "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
               "BLAKE3 of \"abc\"");
}

void test_blake3_hex_parsing() {
  const Blake3Digest digest = blake3_digest(std::string_view("abc"));
  expect(parse_blake3_hex(blake3_hex(digest)) == digest, "hex round trip");
  expect(parse_blake3_prefixed(blake3_prefixed(digest)) == digest,
         "b3: round trip");
  std::string upper = blake3_hex(digest);
  upper[0] = 'A';
  expect(!parse_blake3_hex(upper), "uppercase hex accepted");
  expect(!parse_blake3_hex(blake3_hex(digest).substr(1)), "63 chars accepted");
  expect(!parse_blake3_hex(blake3_hex(digest) + "0"), "65 chars accepted");
  expect(!parse_blake3_hex(std::string(64, 'g')), "non-hex accepted");
  expect(!parse_blake3_prefixed(blake3_hex(digest)), "missing b3: accepted");
  expect(!parse_blake3_prefixed("blake3:" + blake3_hex(digest)),
         "blake3: prefix accepted where b3: is required");
}

void test_encoding_is_sorted_and_compact() {
  const nlohmann::json value = {
      {"zeta", 1}, {"alpha", {{"b", true}, {"a", nullptr}}}, {"mid", "é"}};
  expect_equal(encode_canonical_json(value),
               "{\"alpha\":{\"a\":null,\"b\":true},\"mid\":\"é\",\"zeta\":1}",
               "canonical encoding");
  // Keys sort by UTF-8 byte order, which equals code point order.
  const nlohmann::json unicode = {{"é", 1}, {"z", 2}, {"Z", 3}};
  expect_equal(encode_canonical_json(unicode), "{\"Z\":3,\"z\":2,\"é\":1}",
               "byte-order key sort");
  expect_equal(encode_canonical_json(nlohmann::json{{"c", "\x01"}}),
               "{\"c\":\"\\u0001\"}", "control characters escape as \\u00xx");
}

void test_numbers_have_one_spelling() {
  expect_equal(encode_canonical_json(nlohmann::json::array({0, -1, 12.0, 0.1, 1e21})),
               "[0,-1,12.0,0.1,1e+21]", "number spellings");
  expect(decode_canonical_json("[0,-1,12.0,0.1,1e+21]") ==
             nlohmann::json::array({0, -1, 12.0, 0.1, 1e21}),
         "canonical numbers decode");
  expect_exec_error(ExecErrorCode::non_canonical_json,
                    [] { static_cast<void>(decode_canonical_json("[1e2]")); },
                    "exponent spelling of an integral float");
  expect_exec_error(ExecErrorCode::non_canonical_json,
                    [] { static_cast<void>(decode_canonical_json("[-0]")); },
                    "negative zero integer");
  expect_exec_error(ExecErrorCode::non_canonical_json,
                    [] { static_cast<void>(decode_canonical_json("[0.10]")); },
                    "trailing fraction zero");
}

void test_non_finite_numbers_rejected() {
  for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
    expect_exec_error(ExecErrorCode::non_finite_number,
                      [value] {
                        static_cast<void>(encode_canonical_json(
                            nlohmann::json{{"nested", {value}}}));
                      },
                      "non-finite number encoded");
  }
  // JSON text cannot spell NaN/Infinity; overflowing literals are malformed.
  expect_exec_error(ExecErrorCode::invalid_json,
                    [] { static_cast<void>(decode_canonical_json("[NaN]")); },
                    "NaN literal");
  expect_exec_error(ExecErrorCode::invalid_json,
                    [] { static_cast<void>(decode_canonical_json("[1e400]")); },
                    "overflowing literal");
}

void test_non_canonical_text_rejected() {
  expect(decode_canonical_json("{\"a\":1,\"b\":[true]}") ==
             nlohmann::json{{"a", 1}, {"b", {true}}},
         "canonical object decodes");
  for (const std::string_view text :
       {std::string_view("{\"b\":1,\"a\":2}"), std::string_view("{\"a\": 1}"),
        std::string_view("{\"a\":1}\n"), std::string_view("{\"a\":1,\"a\":2}"),
        std::string_view("[\"\\u00e9\"]"), std::string_view("[\"\\/\"]")}) {
    expect_exec_error(ExecErrorCode::non_canonical_json,
                      [text] { static_cast<void>(decode_canonical_json(text)); },
                      std::string("non-canonical text accepted: ") +
                          std::string(text));
  }
  expect_exec_error(ExecErrorCode::invalid_json,
                    [] { static_cast<void>(decode_canonical_json("{\"a\":")); },
                    "truncated JSON");
  expect_exec_error(ExecErrorCode::invalid_json,
                    [] { static_cast<void>(decode_canonical_json("[\"\xff\"]")); },
                    "invalid UTF-8 in input");
  expect_exec_error(ExecErrorCode::invalid_json,
                    [] {
                      static_cast<void>(encode_canonical_json(
                          nlohmann::json{{"a", std::string("\xff")}}));
                    },
                    "invalid UTF-8 encoded");
}

void test_nesting_is_bounded() {
  const std::string at_limit = std::string(kMaxCanonicalJsonDepth, '[') +
                               std::string(kMaxCanonicalJsonDepth, ']');
  const nlohmann::json nested = decode_canonical_json(at_limit);
  expect_equal(encode_canonical_json(nested), at_limit, "depth at the limit");

  const std::string too_deep = std::string(kMaxCanonicalJsonDepth + 1, '[') +
                               std::string(kMaxCanonicalJsonDepth + 1, ']');
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(decode_canonical_json(too_deep)); },
                    "decode past the depth limit");
  nlohmann::json deep = nested;
  deep = nlohmann::json::array({deep});
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(encode_canonical_json(deep)); },
                    "encode past the depth limit");
  // Brackets inside strings do not count toward depth.
  const std::string in_string = "[\"" + std::string(200, '[') + "\"]";
  expect_equal(encode_canonical_json(decode_canonical_json(in_string)), in_string,
               "brackets inside strings");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec canonical json tests",
      {{"blake3_matches_reference_vectors", test_blake3_matches_reference_vectors},
       {"blake3_hex_parsing", test_blake3_hex_parsing},
       {"encoding_is_sorted_and_compact", test_encoding_is_sorted_and_compact},
       {"numbers_have_one_spelling", test_numbers_have_one_spelling},
       {"non_finite_numbers_rejected", test_non_finite_numbers_rejected},
       {"non_canonical_text_rejected", test_non_canonical_text_rejected},
       {"nesting_is_bounded", test_nesting_is_bounded}});
}
