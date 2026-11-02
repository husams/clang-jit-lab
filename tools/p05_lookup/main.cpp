// p05_lookup -- find declarations in the interpreter's AST with Sema's name
// lookup, and read a variable's current value without value printing
// (Section 5.7).
//
// Usage: build/bin/p05_lookup setup.repl lookup name...
//        build/bin/p05_lookup setup.repl qualified scope::path name...
//        build/bin/p05_lookup setup.repl value variable...
//        build/bin/p05_lookup setup.repl decls
#include "../p05_anatomy/kinds.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"

using namespace clang;

static std::string hex(const void *P) {
  std::string S;
  llvm::raw_string_ostream(S) << llvm::format("0x%llx", (unsigned long long)(uintptr_t)P);
  return S;
}

static std::string typeStr(ASTContext &C, QualType T) { return T.getAsString(C.getPrintingPolicy()); }

static std::string initStr(ASTContext &C, const Expr *E) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  E->printPretty(OS, nullptr, C.getPrintingPolicy());
  return S;
}

// One line about a declaration: what kind it is and what we can say about it.
static std::string describe(Interpreter &I, NamedDecl *ND) {
  ASTContext &C = I.getASTContext();
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << ND->getDeclKindName() << "Decl";
  if (auto *VD = llvm::dyn_cast<VarDecl>(ND)) {
    OS << ": type '" << typeStr(C, VD->getType()) << "'";
    if (VD->hasInit())
      OS << ", init '" << initStr(C, VD->getInit()) << "'";
  } else if (auto *FD = llvm::dyn_cast<FunctionDecl>(ND)) {
    OS << ": type '" << typeStr(C, FD->getType()) << "'";
    if (FD->isExternC())
      OS << ", extern \"C\"";
  } else if (auto *TD = llvm::dyn_cast<TypedefNameDecl>(ND)) {
    OS << ": '" << ND->getName() << "' names '" << typeStr(C, TD->getUnderlyingType()) << "', canonical '"
       << typeStr(C, TD->getUnderlyingType().getCanonicalType()) << "'";
  } else if (auto *RD = llvm::dyn_cast<CXXRecordDecl>(ND)) {
    OS << ": " << (RD->hasDefinition() ? "defined" : "declared only") << ", " << std::distance(RD->field_begin(), RD->field_end())
       << " fields";
  } else if (auto *ED = llvm::dyn_cast<EnumDecl>(ND)) {
    OS << ": underlying '" << typeStr(C, ED->getIntegerType()) << "'";
  } else if (auto *EC = llvm::dyn_cast<EnumConstantDecl>(ND)) {
    OS << ": value " << EC->getInitVal() << " of enum '" << EC->getType().getAsString() << "'";
  } else if (auto *FT = llvm::dyn_cast<FunctionTemplateDecl>(ND)) {
    OS << ": " << FT->getTemplateParameters()->size() << " template parameter(s), "
       << std::distance(FT->specializations().begin(), FT->specializations().end()) << " specialization(s)";
  } else if (auto *NS = llvm::dyn_cast<NamespaceDecl>(ND)) {
    OS << ": '" << NS->getQualifiedNameAsString() << "'";
  }
  return S;
}

// Print what a lookup found. An overload set has several decls.
static void report(Interpreter &I, Sema &S, const std::string &Label, LookupResult &R) {
  llvm::outs() << Label << ": ";
  if (R.empty()) {
    llvm::outs() << "not found\n";
    return;
  }
  if (R.isOverloadedResult()) {
    llvm::outs() << "overload set of " << R.end() - R.begin() << "\n";
    for (NamedDecl *D : R)
      llvm::outs() << "    " << describe(I, D) << "\n";
    return;
  }
  llvm::outs() << describe(I, R.getFoundDecl()) << "\n";
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_lookup: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 2) {
    llvm::errs() << "usage: p05_lookup setup.repl lookup|qualified|decls ...\n";
    return 2;
  }
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0])))
    ExitOnErr(I->ParseAndExecute(Code));

  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();
  std::string Mode = A.Positional[1];

  if (Mode == "lookup") {
    for (size_t K = 2; K < A.Positional.size(); ++K) {
      const std::string &Name = A.Positional[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(), Sema::LookupOrdinaryName);
      S.LookupName(R, S.TUScope);
      report(*I, S, Name, R);
    }
  } else if (Mode == "qualified") {
    // Walk a scope path such as geo::inner, one qualified lookup per step,
    // starting at the translation unit.
    DeclContext *DC = C.getTranslationUnitDecl();
    llvm::SmallVector<llvm::StringRef, 4> Path;
    llvm::StringRef(A.Positional[2]).split(Path, "::");
    for (llvm::StringRef Step : Path) {
      LookupResult R(S, DeclarationName(&C.Idents.get(Step)), SourceLocation(), Sema::LookupNamespaceName);
      S.LookupQualifiedName(R, DC);
      auto *NS = R.empty() ? nullptr : llvm::dyn_cast<NamespaceDecl>(R.getFoundDecl());
      if (!NS) {
        llvm::outs() << "no namespace " << Step << "\n";
        return 1;
      }
      DC = NS;
    }
    for (size_t K = 3; K < A.Positional.size(); ++K) {
      const std::string &Name = A.Positional[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(), Sema::LookupOrdinaryName);
      S.LookupQualifiedName(R, DC);
      report(*I, S, A.Positional[2] + "::" + Name, R);
    }
  } else if (Mode == "value") {
    for (size_t K = 2; K < A.Positional.size(); ++K) {
      const std::string &Name = A.Positional[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(), Sema::LookupOrdinaryName);
      S.LookupName(R, S.TUScope);
      auto *VD = R.empty() ? nullptr : llvm::dyn_cast<VarDecl>(R.getFoundDecl());
      if (!VD) {
        llvm::outs() << Name << ": not a variable\n";
        continue;
      }
      // The address of the variable's storage in the JIT: no value printing involved.
      void *P = ExitOnErr(I->getSymbolAddress(GlobalDecl(VD))).toPtr<void *>();
      QualType T = VD->getType();
      llvm::outs() << Name << ": '" << typeStr(C, T) << "' at " << repllab::maskAddrs(hex(P)) << ", " << C.getTypeSizeInChars(T).getQuantity()
                   << " bytes";
      if (const auto *BT = T->getAs<BuiltinType>()) {
        if (BT->getKind() == BuiltinType::Int)
          llvm::outs() << ", value " << *static_cast<int *>(P);
        else if (BT->getKind() == BuiltinType::Double)
          llvm::outs() << ", value " << llvm::format("%g", *static_cast<double *>(P));
      }
      llvm::outs() << "\n";
      if (Name == "counter") { // write through the address; the interpreter sees it
        *static_cast<int *>(P) = 1000;
        repllab::run(*I, "counter");
      }
    }
  } else if (Mode == "decls") {
    SourceManager &SM = C.getSourceManager();
    TranslationUnitDecl *TU = C.getTranslationUnitDecl();
    llvm::outs() << "getTranslationUnitDecl()->decls(): " << std::distance(TU->decls_begin(), TU->decls_end())
                 << " declaration(s): only the newest input\n";

    // Every input is its own TranslationUnitDecl, chained as redeclarations,
    // newest first. input_line_0 is the interpreter's own runtime prelude.
    std::vector<TranslationUnitDecl *> Parts(TU->redecls().begin(), TU->redecls().end());
    llvm::outs() << "redecls(): " << Parts.size() << " parts\n";
    std::function<void(Decl *)> show = [&](Decl *D) {
      llvm::StringRef File = SM.getFilename(SM.getSpellingLoc(D->getLocation()));
      if (D->isImplicit() || !File.starts_with("input_line_") || File == "input_line_0")
        return;
      if (auto *LS = llvm::dyn_cast<LinkageSpecDecl>(D)) { // extern "C" { ... }
        for (Decl *Inner : LS->decls())
          show(Inner);
      } else if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        llvm::outs() << "  " << File << "  " << ND->getNameAsString() << "  " << describe(*I, ND) << "\n";
      }
    };
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It)
      for (Decl *D : (*It)->decls())
        show(D);

    // The PTU that Parse() returns is the newest part.
    PartialTranslationUnit &PTU = ExitOnErr(I->Parse("int late = 1;"));
    llvm::outs() << "Parse(): PTU.TUPart is the new TU: " << (PTU.TUPart == C.getTranslationUnitDecl()) << ", with "
                 << std::distance(PTU.TUPart->decls_begin(), PTU.TUPart->decls_end()) << " declaration(s)\n";
  }
  return 0;
}
