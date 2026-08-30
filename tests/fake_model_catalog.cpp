#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
  if (argc != 4 || std::string_view(argv[1]) != "models" ||
      std::string_view(argv[2]) != "opencode-go" ||
      std::string_view(argv[3]) != "--verbose") {
    return 2;
  }

  std::cout << R"(opencode-go/manual-refresh-model
{
  "id": "manual-refresh-model",
  "name": "Manual Refresh Model",
  "api": {"npm": "@ai-sdk/openai"},
  "limit": {"context": 200000, "output": 32000},
  "capabilities": {"temperature": false},
  "variants": {"low": {"reasoningEffort": "low"}}
}
)";
  return 0;
}
