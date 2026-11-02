#include "repllab.h"

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError Check("p03_library: ");
  if (argc != 2) {
    llvm::errs() << "usage: p03_library path/to/library.dylib\n";
    return 2;
  }
  auto Engine = Check(repllab::makeInterpreter());
  Check(Engine->LoadDynamicLibrary(argv[1]));
  Check(Engine->ParseAndExecute("extern \"C\" int lab_triple(int);"));
  clang::Value Result;
  Check(Engine->ParseAndExecute("lab_triple(14)", &Result));
  llvm::outs() << "library result = " << Result.convertTo<int>() << "\n";
}
