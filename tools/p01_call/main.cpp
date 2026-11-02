#include <memory>

#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "repllab.h"

int main() {
  repllab::init();
  llvm::ExitOnError Check("p01_call: ");
  auto Engine = Check(repllab::makeInterpreter());
  Check(Engine->ParseAndExecute(
      "extern \"C\" int run(int x) { return x * 7; }"));
  auto Address = Check(Engine->getSymbolAddress("run"));
  auto Run = Address.toPtr<int (*)(int)>();
  llvm::outs() << "run(6) = " << Run(6) << "\n";
  return 0;
}
