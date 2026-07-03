#include "ocr_generation_internal.hpp"

#include <cctype>
#include <regex>
#include <utility>

namespace svp::vision::ocr_generation_internal {
namespace {

bool looks_like_date_context(const std::string& text) {
  static const std::regex date_re(
      R"((january|february|march|april|may|june|july|august|september|october|november|december)\s+\d{1,2}(?:,\s*|\s+)\d{2,4})",
      std::regex_constants::icase);
  return std::regex_search(text, date_re);
}

bool overlaps_any(std::size_t pos,
                  std::size_t end_pos,
                  const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
  for (const auto& r : ranges) {
    if (pos < r.second && end_pos > r.first) return true;
  }
  return false;
}

std::string normalize_numeric_token(const std::string& token) {
  std::string normalized;
  normalized.reserve(token.size());
  for (const char c : token) {
    if (c == ',') continue;
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    normalized += c;
  }
  return normalized;
}

}  // namespace

std::vector<ParsedNumber> parse_numeric_values(const std::string& raw_text,
                                                double confidence) {
  std::vector<ParsedNumber> results;
  static const std::regex currency_re(
      R"(\$((?:\d{1,3}(?:,\d{3})+|\d+)(?:\.\s*\d+)?))");
  static const std::regex grouped_number_re(
      R"(\b(\d{1,3}(?:,\d{3})+(?:\.\d+)?)\b)");
  static const std::regex decimal_re(R"(\b(\d+\.\d+)\b)");
  static const std::regex integer_re(R"(\b(\d+)\b)");

  std::string::const_iterator search_start = raw_text.cbegin();
  std::smatch match;
  std::vector<std::pair<std::size_t, std::size_t>> matched_ranges;

  while (std::regex_search(search_start, raw_text.cend(), match, currency_re)) {
    ParsedNumber num;
    num.raw_text = match[0].str();
    num.normalized_text = normalize_numeric_token(match[1].str());
    num.numeric_value = num.normalized_text;
    num.number_kind = "decimal";
    num.unit = "currency_unknown";
    num.confidence = confidence;
    results.push_back(std::move(num));

    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t len = match[0].length();
    matched_ranges.push_back({pos, pos + len});
    search_start = match[0].second;
  }

  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, grouped_number_re)) {
    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t end_pos = pos + match[0].length();
    if (!overlaps_any(pos, end_pos, matched_ranges)) {
      ParsedNumber num;
      num.raw_text = match[0].str();
      num.normalized_text = normalize_numeric_token(match[1].str());
      num.number_kind = "decimal";
      num.numeric_value = num.normalized_text;
      num.confidence = confidence;
      results.push_back(std::move(num));
      matched_ranges.push_back({pos, end_pos});
    }
    search_start = match[0].second;
  }

  search_start = raw_text.cbegin();
  while (std::regex_search(search_start, raw_text.cend(), match, decimal_re)) {
    std::size_t pos = match[0].first - raw_text.cbegin();
    std::size_t end_pos = pos + match[0].length();
    if (!overlaps_any(pos, end_pos, matched_ranges)) {
      ParsedNumber num;
      num.raw_text = match[0].str();
      num.normalized_text = match[0].str();
      num.number_kind = "decimal";
      num.numeric_value = match[0].str();
      num.confidence = confidence;
      results.push_back(std::move(num));
      matched_ranges.push_back({pos, end_pos});
    }
    search_start = match[0].second;
  }

  if (looks_like_date_context(raw_text)) {
    search_start = raw_text.cbegin();
    while (std::regex_search(search_start, raw_text.cend(), match, integer_re)) {
      std::size_t pos = match[0].first - raw_text.cbegin();
      std::size_t end_pos = pos + match[0].length();
      if (!overlaps_any(pos, end_pos, matched_ranges)) {
        ParsedNumber num;
        num.raw_text = match[0].str();
        num.normalized_text = match[0].str();
        num.number_kind = "integer";
        num.numeric_value = match[0].str();
        num.confidence = confidence;
        results.push_back(std::move(num));
        matched_ranges.push_back({pos, end_pos});
      }
      search_start = match[0].second;
    }
  }

  return results;
}

}  // namespace svp::vision::ocr_generation_internal
