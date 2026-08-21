// Probe: C++26 #embed of a small file, per D5 (migrations/templates embed).
#include <cstdio>
#include <string_view>

constexpr unsigned char kEmbedded[] = {
#embed "probe_embed_data.txt"
};

int main() {
  std::string_view text(reinterpret_cast<const char*>(kEmbedded), sizeof(kEmbedded));
  std::printf("embedded %zu bytes: %.*s", sizeof(kEmbedded), static_cast<int>(text.size()), text.data());
  return 0;
}
