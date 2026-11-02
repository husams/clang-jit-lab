// p05_anatomy -- evaluate every input of a .repl file with ParseAndExecute(code, &V)
// and print one table row per input: the Value's kind, type and flags (Section 5.1).
//
// Usage: build/bin/p05_anatomy inputs.repl [-- compiler flags]
//        build/bin/p05_anatomy reuse        (one Value reused for several inputs)
#include "../p05_anatomy/kinds.h"

#include "clang/AST/ASTContext.h"

using namespace clang;

static std::string pad(std::string S, size_t N) {
  if (S.size() < N)
    S.append(N - S.size(), ' ');
  return S;
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_anatomy: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));

  if (!A.Positional.empty() && A.Positional[0] == "reuse") {
    ExitOnErr(I->ParseAndExecute("int x = 42; void nothing() {}"));
    // One Value reused for several inputs: only an input that produces a value
    // writes to it. A declaration leaves the previous contents alone.
    llvm::outs() << "\nreusing one Value:\n";
    Value V;
    for (const char *Code : {"x", "int y = 1;", "nothing()", "y + 1", "int z = 2;"}) {
      if (!repllab::run(*I, Code, &V))
        continue;
      llvm::outs() << "  after " << pad(Code, 12) << "-> " << pad(p05::kindName(V.getKind()), 15)
                   << (V.hasValue() ? repllab::valueString(V) : std::string("(no data)\n"));
    }
    return 0;
  }

  llvm::outs() << pad("input", 38) << pad("isValid", 8) << pad("kind", 15) << pad("hasValue", 9)
               << pad("manual", 7) << "type\n";
  for (const std::string &File : A.Positional) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      Value V; // a fresh Value for every input: default constructed = invalid
      if (!repllab::run(*I, Code, &V))
        continue;
      std::string Type = V.isValid() ? V.getType().getAsString(V.getASTContext().getPrintingPolicy()) : "-";
      llvm::outs() << pad(Code, 38) << pad(V.isValid() ? "yes" : "no", 8) << pad(p05::kindName(V.getKind()), 15)
                   << pad(V.hasValue() ? "yes" : "no", 9) << pad(V.isManuallyAlloc() ? "yes" : "no", 7) << Type
                   << "\n";
    }
  }

  return 0;
}
