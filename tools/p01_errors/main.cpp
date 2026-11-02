#include <memory>
#include <utility>

#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "repllab.h"

int main() {
  repllab::init();
  llvm::ExitOnError Check("p01_errors: ");
  auto Engine = Check(repllab::makeInterpreter());
  if (llvm::Error E = Engine->ParseAndExecute("int broken = ;"))
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "input: ");

  // A simple failed input is followed by a separate valid input.
  clang::Value Result;
  Check(Engine->ParseAndExecute("6 * 7", &Result));
  if (!Result.isValid() || Result.isVoid())
    return 1;
  llvm::outs() << "after error = " << Result.convertTo<int>() << "\n";
  return 0;
}
