#include "ocr_generation_internal.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace svp::vision::ocr_generation_internal {
namespace {

std::string trim(const std::string& s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

}  // namespace

nlohmann::json make_ocr_processor_provenance(
    const std::string& id,
    const std::string& type,
    const std::string& version,
    const std::string& runtime,
    const std::string& status,
    const std::string& note) {
  return {
      {"id", sanitize_utf8(id)},
      {"processor_type", sanitize_utf8(type)},
      {"processor_version", sanitize_utf8(version)},
      {"runtime", sanitize_utf8(runtime)},
      {"execution_provider", "cpu"},
      {"model_refs", nlohmann::json::array()},
      {"status", sanitize_utf8(status)},
      {"note", sanitize_utf8(note)},
  };
}

std::string normalize_text(const std::string& raw) {
  std::string result = trim(raw);
  std::string collapsed;
  bool prev_space = false;
  for (char c : result) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!prev_space) collapsed += ' ';
      prev_space = true;
    } else {
      collapsed += c;
      prev_space = false;
    }
  }
  std::string no_punct_space;
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    if (i > 0 && collapsed[i] == ' ' &&
        (collapsed[i - 1] == '.' || collapsed[i - 1] == ',' ||
         collapsed[i - 1] == ';' || collapsed[i - 1] == ':' ||
         collapsed[i - 1] == '$' || collapsed[i - 1] == '!')) {
      continue;
    }
    no_punct_space += collapsed[i];
  }
  return lower(no_punct_space);
}

std::string alphanumeric_key(const std::string& text) {
  std::string key;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      key += std::tolower(static_cast<unsigned char>(c));
    }
  }
  return key;
}

std::string pad_id(const std::string& prefix, int index, int width) {
  std::ostringstream oss;
  oss << prefix << std::setw(width) << std::setfill('0') << index;
  return oss.str();
}

bool roi_text_is_better(const std::string& current_text,
                        const std::string& roi_text) {
  const std::string current_key = alphanumeric_key(normalize_text(current_text));
  const std::string roi_key = alphanumeric_key(normalize_text(roi_text));
  if (roi_key.size() < 3) return false;
  if (current_key.empty()) return true;
  if (roi_key == current_key) return false;
  if (roi_key.size() <= current_key.size() + 2) return false;
  if (current_key.size() <= 3) return true;
  return roi_key.find(current_key) != std::string::npos;
}

}  // namespace svp::vision::ocr_generation_internal
