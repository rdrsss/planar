// Probe: -std=c++2c is accepted and a genuine C++26-only feature works.
// Uses pack indexing (P2662R3, new in C++26): `pack...[index]`.
#include <cstdio>

template <typename... Ts> constexpr auto first_of(Ts... vals) {
  return vals...[0];
}

int main() {
  static_assert(first_of(1, 2, 3) == 1, "pack indexing must select the first element");
  std::printf("pack indexing ok: %d\n", first_of(7, 8, 9));
  return 0;
}
