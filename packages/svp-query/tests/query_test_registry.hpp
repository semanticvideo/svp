#pragma once

#include <string>

using QueryTestFn = void (*)();

void register_query_test(std::string name, QueryTestFn fn);

int run_registered_query_tests();

struct QueryTestRegistrar {
  QueryTestRegistrar(const std::string& name, QueryTestFn fn);
};

#define REGISTER_QUERY_TEST(name) \
  static QueryTestRegistrar registrar_##name(#name, name);
