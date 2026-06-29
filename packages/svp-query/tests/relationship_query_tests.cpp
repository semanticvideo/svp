#include "query_test_registry.hpp"
#include "fixtures/relationship_package_fixture.hpp"
#include "svp/query/query_ops.hpp"

#include <cassert>
#include <iostream>

void test_relationship_summary() {
  const auto pkg = create_test_package_with_relationships();
  auto summary = svp::query::relationship_summary(pkg);

  assert(summary.present);
  assert(summary.readable);
  assert(summary.total_count == 5);
  assert(summary.support_count == 2);
  assert(summary.semantic_count == 2);
  assert(summary.unknown_count == 1);

  std::cout << "test_relationship_summary: passed\n";
}

REGISTER_QUERY_TEST(test_relationship_summary)

void test_list_relationships_all() {
  const auto pkg = create_test_package_with_relationships();
  auto all = svp::query::list_relationships(pkg, std::nullopt, 100);

  assert(all.size() == 5);

  std::cout << "test_list_relationships_all: passed\n";
}

REGISTER_QUERY_TEST(test_list_relationships_all)

void test_list_relationships_filtered_by_support() {
  const auto pkg = create_test_package_with_relationships();
  auto support_only = svp::query::list_relationships(pkg, std::string{"support"}, 100);

  assert(support_only.size() == 2);
  for (const auto& rel : support_only) {
    assert(rel.relationship_class == "support");
  }

  std::cout << "test_list_relationships_filtered_by_support: passed\n";
}

REGISTER_QUERY_TEST(test_list_relationships_filtered_by_support)

void test_list_relationships_filtered_by_semantic() {
  const auto pkg = create_test_package_with_relationships();
  auto semantic_only = svp::query::list_relationships(pkg, std::string{"semantic"}, 100);

  assert(semantic_only.size() == 2);
  for (const auto& rel : semantic_only) {
    assert(rel.relationship_class == "semantic");
  }

  std::cout << "test_list_relationships_filtered_by_semantic: passed\n";
}

REGISTER_QUERY_TEST(test_list_relationships_filtered_by_semantic)

void test_list_relationships_filtered_by_unknown() {
  const auto pkg = create_test_package_with_relationships();
  auto unknown_only = svp::query::list_relationships(pkg, std::string{"unknown"}, 100);

  assert(unknown_only.size() == 1);
  assert(unknown_only[0].relationship_class == "unknown");
  assert(unknown_only[0].record.value("type", "") == "totally_unknown_type");

  std::cout << "test_list_relationships_filtered_by_unknown: passed\n";
}

REGISTER_QUERY_TEST(test_list_relationships_filtered_by_unknown)

void test_json_type_field_canonical_in_query() {
  const auto pkg = create_test_package_with_relationships();
  auto all = svp::query::list_relationships(pkg, std::nullopt, 100);

  for (const auto& rel : all) {
    assert(rel.record.contains("type"));
    assert(!rel.record.contains("relationship_type"));
  }

  std::cout << "test_json_type_field_canonical_in_query: passed\n";
}

REGISTER_QUERY_TEST(test_json_type_field_canonical_in_query)
