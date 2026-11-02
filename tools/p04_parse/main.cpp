// p04_parse -- Parse versus Execute (Section 4.5). Runs a fixed script of
// experiments on the PartialTranslationUnit returned by Interpreter::Parse.
//
// Usage: build/bin/p04_parse
#include "clang/AST/Decl.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

using namespace clang;

static const char *yesNo(bool B) { return B ? "yes" : "no"; }

static void describe(const char *When, PartialTranslationUnit &PTU) {
  unsigned Decls = std::distance(PTU.TUPart->decls_begin(), PTU.TUPart->decls_end());
  llvm::outs() << "  " << When << ": TUPart decls=" << Decls
               << " TheModule non-null=" << yesNo(PTU.TheModule != nullptr);
  if (PTU.TheModule)
    llvm::outs() << " name=" << PTU.TheModule->getName();
  llvm::outs() << "\n";
}

// ORC lists the symbols that failed to materialize in hash order; keep the
// message but drop the set so the output is the same on every run.
static std::string brief(llvm::Error E) {
  std::string Msg = llvm::toString(std::move(E));
  size_t Brace = Msg.find("{ (");
  return Brace == std::string::npos ? Msg : Msg.substr(0, Brace) + "{ ... }";
}

int main() {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_parse: ");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter());
  ExitOnErr(I->ParseAndExecute("#include <cstdio>"));

  llvm::outs() << "== 1. a statement that prints\n";
  PartialTranslationUnit &S = ExitOnErr(I->Parse("printf(\"hello from the JIT\\n\");"));
  describe("after Parse  ", S);
  llvm::outs() << "  (nothing has run yet)\n";
  ExitOnErr(I->Execute(S));
  describe("after Execute", S);

  llvm::outs() << "== 2. a definition, parsed but not executed\n";
  PartialTranslationUnit &F = ExitOnErr(I->Parse("int triple(int v) { return 3 * v; }"));
  describe("after Parse  ", F);
  llvm::outs() << "  getSymbolAddress(_Z6triplei): ";
  if (auto Addr = I->getSymbolAddress("_Z6triplei"))
    llvm::outs() << "found\n";
  else
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  llvm::outs() << "  a later ParseAndExecute(\"triple(14)\"):\n";
  if (llvm::Error E = I->ParseAndExecute("triple(14)"))
    llvm::outs() << "  error: " << brief(std::move(E)) << "\n";
  llvm::outs() << "  Execute the pending definition:\n";
  if (llvm::Error E = I->Execute(F))
    llvm::outs() << "  error: " << brief(std::move(E)) << "\n";
  llvm::outs() << "  getSymbolAddress(_Z6triplei) now: ";
  if (auto Addr = I->getSymbolAddress("_Z6triplei"))
    llvm::outs() << "found\n";
  else
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  llvm::outs() << "  ParseAndExecute(\"triple(14)\") again:\n";
  repllab::run(*I, "triple(14)");

  llvm::outs() << "== 3. parse two inputs, then execute them in order\n";
  PartialTranslationUnit &D = ExitOnErr(I->Parse("int quad(int v) { return 4 * v; }"));
  PartialTranslationUnit &U = ExitOnErr(I->Parse("printf(\"quad(5) = %d\\n\", quad(5));"));
  ExitOnErr(I->Execute(D));
  ExitOnErr(I->Execute(U));
  return 0;
}
