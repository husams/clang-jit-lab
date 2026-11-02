// p04_errors -- three ways to consume an llvm::Error, and the exact messages
// of the errors the Interpreter API produces (Section 4.4).
//
// Usage: build/bin/p04_errors
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

using namespace clang;

int main() {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_errors: ");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter());

  // An llvm::Error must be consumed exactly once; destroying an unchecked one
  // aborts the process in assertion builds. Each style below consumes it.
  llvm::outs() << "-- toString\n";
  if (llvm::Error E = I->ParseAndExecute("int a = ;"))
    llvm::outs() << "message: " << llvm::toString(std::move(E)) << "\n";

  llvm::outs() << "-- handleAllErrors\n";
  if (llvm::Error E = I->ParseAndExecute("int b = nope;"))
    llvm::handleAllErrors(std::move(E), [](const llvm::ErrorInfoBase &EIB) {
      llvm::outs() << "message: " << EIB.message() << "\n";
    });

  llvm::outs() << "-- logAllUnhandledErrors\n";
  if (llvm::Error E = I->ParseAndExecute("int c = 'x"))
    llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "error: ");

  llvm::outs() << "-- Expected from Parse\n";
  llvm::Expected<PartialTranslationUnit &> PTU = I->Parse("int d = nope;");
  llvm::outs() << "Parse succeeded: " << (PTU ? "yes" : "no") << "\n";
  if (!PTU)
    llvm::outs() << "message: " << llvm::toString(PTU.takeError()) << "\n";

  llvm::outs() << "-- Undo\n";
  llvm::outs() << "Undo(1) on a fresh interpreter: ";
  llvm::outs() << llvm::toString(I->Undo()) << "\n";
  repllab::run(*I, "int e = 1;");
  repllab::run(*I, "int f = 2;");
  llvm::outs() << "Undo(5) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(5)) << "\n";
  llvm::outs() << "Undo(2) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(2)) << "\n";
  llvm::outs() << "(llvm::toString of a success is the empty string)\n";

  llvm::outs() << "-- the interpreter still works\n";
  repllab::run(*I, "int g = 7; g");
  return 0;
}
