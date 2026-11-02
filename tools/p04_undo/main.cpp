// p04_undo -- Interpreter::Undo from the API (Section 4.8): what disappears
// from the AST and from the JIT, what error you get, and the lower bound.
//
// Usage: build/bin/p04_undo [input ...]
//   without arguments: the fixed walk-through; with arguments: run them as
//   inputs, then count how many Undo() calls succeed.
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

using namespace clang;

// Is `Name` still visible in the session's translation unit?
static bool declared(Interpreter &I, llvm::StringRef Name) {
  ASTContext &Ctx = I.getASTContext();
  DeclarationName DN(&Ctx.Idents.get(Name));
  return !Ctx.getTranslationUnitDecl()->lookup(DN).empty();
}

static void state(Interpreter &I, const char *When, llvm::StringRef Name, llvm::StringRef IRName) {
  llvm::outs() << "  " << When << ": declared=" << (declared(I, Name) ? "yes" : "no") << " JIT symbol ";
  if (auto Addr = I.getSymbolAddress(IRName))
    llvm::outs() << "found\n";
  else
    llvm::outs() << "missing (" << llvm::toString(Addr.takeError()) << ")\n";
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_undo: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  if (!A.Positional.empty()) { // run the arguments as inputs, then Undo() until it fails
    std::unique_ptr<Interpreter> J = ExitOnErr(repllab::makeInterpreter(A.Compiler));
    for (const std::string &Code : A.Positional)
      repllab::run(*J, Code);
    unsigned Undone = 0;
    while (true) {
      llvm::Error E = J->Undo();
      if (E) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "Undo() succeeded " << Undone << " times\n";
    return 0;
  }

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter());

  llvm::outs() << "== undo a definition\n";
  ExitOnErr(I->ParseAndExecute("int base = 1;"));
  ExitOnErr(I->ParseAndExecute("int f() { return base + 1; }"));
  state(*I, "before Undo", "f", "_Z1fv");
  ExitOnErr(I->Undo());
  state(*I, "after Undo ", "f", "_Z1fv");
  llvm::outs() << "  base is untouched: declared=" << (declared(*I, "base") ? "yes" : "no") << "\n";
  llvm::outs() << "  calling f():\n";
  repllab::run(*I, "f()");

  llvm::outs() << "== redefine after undo\n";
  ExitOnErr(I->ParseAndExecute("int f() { return base + 100; }"));
  repllab::run(*I, "f()");

  llvm::outs() << "== undo a global with a different type, then redefine it\n";
  ExitOnErr(I->Undo(2)); // the new f() and the f() call that printed 101
  ExitOnErr(I->Undo());  // int base
  repllab::run(*I, "double base = 2.5;");
  repllab::run(*I, "base");

  llvm::outs() << "== the lower bound\n";
  // Inputs so far: int base (undone), double base, base  => 2 live inputs.
  llvm::outs() << "Undo(3): " << llvm::toString(I->Undo(3)) << "\n";
  llvm::outs() << "Undo(2): " << llvm::toString(I->Undo(2)) << "|\n";
  llvm::outs() << "Undo(1): " << llvm::toString(I->Undo(1)) << "\n";
  llvm::outs() << "the prelude is still there:\n";
  repllab::run(*I, "1 + 1");
  return 0;
}
