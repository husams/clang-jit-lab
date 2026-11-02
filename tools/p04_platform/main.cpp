// p04_platform -- print what repllab::platformArgs() adds to every
// interpreter this lab creates (Section 4.3).
//
// Usage: build/bin/p04_platform          (REPLLAB_RAW=1 build/bin/p04_platform)
#include "repllab.h"

int main() {
  llvm::outs() << "raw mode: " << (repllab::rawMode() ? "yes" : "no") << "\n";
  std::vector<std::string> Args = repllab::platformArgs();
  llvm::outs() << Args.size() << " platform arguments\n";
  for (const std::string &S : Args)
    llvm::outs() << "  " << S << "\n";
  return 0;
}
