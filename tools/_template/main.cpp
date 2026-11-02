// pNN_name -- one line: what this tool demonstrates (Section N.M).
//
// Usage: build/bin/pNN_name [inputs...] [-- compiler flags]
//
// Copy this directory to tools/pNN_<name>/ and run scripts/build.sh pNN_<name>.
#include "repllab.h"

using namespace clang;

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("pNN_name: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));

  for (const std::string &Code : A.Positional)
    repllab::run(*I, Code);
  return 0;
}
