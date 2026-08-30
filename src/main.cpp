#include "zed/app/application.hpp"
#include "zed/subagents/worker.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kVersion = ZEDA_VERSION;

void print_usage(std::ostream &output) {
  output << "Usage: zeda [--help] [--version]\n"
         << "\n"
         << "Start the zeda coding agent in the current directory.\n"
         << "\n"
         << "Options:\n"
         << "  -h, --help     Show this help.\n"
         << "  -V, --version  Show the installed version.\n";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc > 1) {
    const std::string_view argument(argv[1]);
    if (argc == 2 && argument == "--subagent-worker") {
      return zed::subagents::run_explorer_worker(std::cin, std::cout,
                                                 std::cerr);
    }
    if (argc == 2 && (argument == "--help" || argument == "-h")) {
      print_usage(std::cout);
      return 0;
    }
    if (argc == 2 && (argument == "--version" || argument == "-V")) {
      std::cout << "zeda " << kVersion << '\n';
      return 0;
    }
    std::cerr << "unknown argument: " << argument << "\n\n";
    print_usage(std::cerr);
    return 2;
  }
  return zed::app::run_application(argv[0], kVersion);
}
