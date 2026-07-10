#include "embedded_svpi_test_declarations.hpp"

#include <exception>
#include <iostream>

int main() {
  try {
    run_embedded_svpi_reader_tests();
    run_embedded_svpi_writer_tests();
    std::cout << "All embedded SVPI MP4 tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
