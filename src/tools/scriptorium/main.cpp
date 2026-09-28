import std;
import planar.tools.scriptorium;

auto main(int argc, char** argv) -> int {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);
  return planar::tools::scriptorium::run(args, std::cout, std::cerr);
}
