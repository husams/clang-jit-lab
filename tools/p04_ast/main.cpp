// p04_ast -- look at the AST a PartialTranslationUnit holds (Section 4.6).
//
// Usage: build/bin/p04_ast inputs.repl [N ...] [-- compiler flags]
//   Lists the declarations of every input's PTU. For each input number N it
//   also dumps the declarations with Decl::dump(). Finally compares the
//   TranslationUnitDecls of the last two inputs.
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Regex.h"

#include "repllab.h"

#include <set>

using namespace clang;

static std::string dumpToString(const Decl *D) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  D->dump(OS);
  // The synthesized value-printing call embeds raw addresses as decimal literals.
  static const llvm::Regex Big("(unsigned long long' )[0-9]{8,}");
  std::string Err;
  while (Big.match(S))
    S = Big.sub("\\1<addr>", S, &Err);
  return repllab::maskAddrs(S);
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_ast: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.empty()) {
    llvm::errs() << "usage: p04_ast inputs.repl [N ...] [-- compiler flags]\n";
    return 2;
  }
  std::set<unsigned> DumpInputs;
  for (size_t K = 1; K < A.Positional.size(); ++K)
    DumpInputs.insert(std::stoi(A.Positional[K]));

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));

  std::vector<TranslationUnitDecl *> TUs;
  unsigned N = 0;
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0]))) {
    ++N;
    llvm::outs() << "[" << N << "] " << Code << "\n";
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::outs() << "    parse failed: " << llvm::toString(PTU.takeError()) << "\n";
      continue;
    }
    TUs.push_back(PTU->TUPart);
    if (PTU->TUPart->decls_empty())
      llvm::outs() << "    (no declarations)\n";
    for (Decl *D : PTU->TUPart->decls()) {
      llvm::outs() << "    " << D->getDeclKindName();
      if (auto *ND = llvm::dyn_cast<NamedDecl>(D))
        llvm::outs() << " " << ND->getQualifiedNameAsString();
      llvm::outs() << "\n";
      if (DumpInputs.count(N))
        llvm::outs() << dumpToString(D);
    }
    if (PTU->TheModule)
      ExitOnErr(I->Execute(*PTU));
  }

  if (TUs.size() >= 2) {
    TranslationUnitDecl *Prev = TUs[TUs.size() - 2], *Last = TUs.back();
    llvm::outs() << "---\n";
    llvm::outs() << "last two inputs have the same TranslationUnitDecl: " << (Prev == Last ? "yes" : "no") << "\n";
    llvm::outs() << "their getCanonicalDecl() are the same: "
                 << (Prev->getCanonicalDecl() == Last->getCanonicalDecl() ? "yes" : "no") << "\n";
    llvm::outs() << "ASTContext::getTranslationUnitDecl() is the last input's: "
                 << (I->getASTContext().getTranslationUnitDecl() == Last ? "yes" : "no") << "\n";
    llvm::outs() << "the canonical decl is one of the inputs' TUs: "
                 << (llvm::is_contained(TUs, Last->getCanonicalDecl()) ? "yes" : "no") << "\n";
  }
  return 0;
}
