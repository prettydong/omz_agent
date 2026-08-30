#include <cstdlib>
#include <fstream>

int main() {
  const auto *marker = std::getenv("ZED_TEST_CLANGD_ENV_MARKER");
  if (marker == nullptr)
    return 2;
  const bool clean = std::getenv("OPENAI_API_KEY") == nullptr &&
                     std::getenv("OPENCODE_GO_API_KEY") == nullptr &&
                     std::getenv("ANTHROPIC_API_KEY") == nullptr &&
                     std::getenv("AWS_SECRET_ACCESS_KEY") == nullptr &&
                     std::getenv("NPM_TOKEN") == nullptr &&
                     std::getenv("DATABASE_URL") == nullptr;
  std::ofstream output(marker);
  output << (clean ? "clean" : "leaked");
  return clean ? 0 : 3;
}
