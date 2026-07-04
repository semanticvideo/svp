#include "cli_elapsed.hpp"

#include <iomanip>
#include <sstream>

std::string format_elapsed_duration(std::chrono::steady_clock::duration duration) {
  using namespace std::chrono;

  const auto total_seconds = duration_cast<seconds>(duration).count();
  const auto hours = total_seconds / 3600;
  const auto minutes = (total_seconds % 3600) / 60;
  const auto seconds_part = total_seconds % 60;

  std::ostringstream out;
  if (hours > 0) {
    out << hours << "h";
    if (minutes > 0) {
      out << " " << std::setw(2) << std::setfill('0') << minutes << "m";
    }
    out << " " << std::setw(2) << std::setfill('0') << seconds_part << "s";
    return out.str();
  }
  if (minutes > 0) {
    out << minutes << "m " << std::setw(2) << std::setfill('0')
        << seconds_part << "s";
    return out.str();
  }
  out << seconds_part << "s";
  return out.str();
}
