#include <unistd.h>

#include <iostream>

#include "flatfiledb/repl.h"

int main() {
  flatfiledb::cli::RunRepl(std::cin, std::cout, ::isatty(STDIN_FILENO) != 0);
  return 0;
}
