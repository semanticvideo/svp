#include "query_test_registry.hpp"

#include <iostream>
#include <utility>
#include <vector>

namespace {

std::vector<std::pair<std::string, QueryTestFn>>& test_registry() {
  static std::vector<std::pair<std::string, QueryTestFn>> registry;
  return registry;
}

}  // namespace

void register_query_test(std::string name, QueryTestFn fn) {
  test_registry().push_back({std::move(name), fn});
}

int run_registered_query_tests() {
  for (const auto& [name, fn] : test_registry()) {
    fn();
  }
  std::cout << "All svp-query tests passed.\n";
  return 0;
}

QueryTestRegistrar::QueryTestRegistrar(const std::string& name, QueryTestFn fn) {
  register_query_test(name, fn);
}
