#include "../src/ocr_generation/ocr_generation_internal.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

const svp::vision::ocr_generation_internal::ParsedNumber& require_single_number(
    const std::string& text) {
  static std::vector<svp::vision::ocr_generation_internal::ParsedNumber> parsed;
  parsed = svp::vision::ocr_generation_internal::parse_numeric_values(text, 0.9);
  require(parsed.size() == 1, "expected exactly one parsed numeric value for " + text);
  return parsed.front();
}

void test_comma_grouped_currency() {
  const auto& sale_price = require_single_number("$1,450,000");
  require(sale_price.raw_text == "$1,450,000", "raw currency text should be preserved");
  require(sale_price.numeric_value == "1450000", "currency commas should be removed");
  require(sale_price.normalized_text == "1450000", "normalized currency should remove commas");
  require(sale_price.unit == "currency_unknown", "currency unit should be preserved");

  require(require_single_number("$4,997").numeric_value == "4997",
          "$4,997 should parse as 4997");
  require(require_single_number("$280,000").numeric_value == "280000",
          "$280,000 should parse as 280000");
}

void test_comma_grouped_number() {
  const auto& count = require_single_number("Audience size 1,450,000");
  require(count.raw_text == "1,450,000", "raw grouped number text should be preserved");
  require(count.numeric_value == "1450000", "grouped number commas should be removed");
  require(count.unit.empty(), "non-currency grouped number should not get a unit");
}

void test_existing_decimal_currency() {
  const auto& price = require_single_number("$9.5");
  require(price.raw_text == "$9.5", "raw decimal currency should be preserved");
  require(price.numeric_value == "9.5", "$9.5 should keep decimal behavior");
  require(price.unit == "currency_unknown", "decimal currency unit should be preserved");
}

}  // namespace

int main() {
  test_comma_grouped_currency();
  test_comma_grouped_number();
  test_existing_decimal_currency();
  std::cout << "All OCR numeric parser tests passed.\n";
  return 0;
}
