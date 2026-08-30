#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
  if (argc != 4 || std::string_view(argv[1]) != "models" ||
      std::string_view(argv[2]) != "opencode-go" ||
      std::string_view(argv[3]) != "--verbose") {
    return 2;
  }

  const char *marker_path = std::getenv("ZEDA_FAKE_DISCOVERY_MARKER");
  if (marker_path == nullptr || *marker_path == '\0')
    return 3;
  std::ofstream marker(marker_path, std::ios::app);
  if (!marker)
    return 4;
  marker << "called\n";
  marker.close();
  if (!marker)
    return 5;

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
