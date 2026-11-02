#include <memory>

#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "repllab.h"

int main() {
  repllab::init();
  llvm::ExitOnError Check("p01_capture: ");
  auto Engine = Check(repllab::makeInterpreter());
  Check(Engine->ParseAndExecute(
      "extern \"C\" int run(int x) { return x * 7; }"));
  clang::Value Result;
  Check(Engine->ParseAndExecute("run(5)", &Result));
  if (Result.isValid() && !Result.isVoid())
    llvm::outs() << "captured = " << Result.convertTo<int>() << "\n";
  return 0;
}
