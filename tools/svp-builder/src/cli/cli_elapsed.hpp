#pragma once

#include <chrono>
#include <string>

std::string format_elapsed_duration(std::chrono::steady_clock::duration duration);

