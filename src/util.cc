#include "util.h"

std::vector<char*> MakeArgV(std::vector<std::string>* args) {
  std::vector<char*> argv;
  argv.reserve(args->size() + 1);
  for (auto& arg : *args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}
