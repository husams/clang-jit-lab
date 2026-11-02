// p05_layout -- how a type is spelled, and where the bytes of a record are
// (Section 5.8).
//
// Usage: build/bin/p05_layout setup.repl types var...
//        build/bin/p05_layout setup.repl layout Record...
//        build/bin/p05_layout setup.repl template Name
#include "../p05_anatomy/kinds.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"

#include <algorithm>

using namespace clang;

static std::string str(ASTContext &C, QualType T) { return T.getAsString(C.getPrintingPolicy()); }

static NamedDecl *lookup(Sema &S, llvm::StringRef Name) {
  ASTContext &C = S.getASTContext();
  LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(), Sema::LookupOrdinaryName);
  S.LookupName(R, S.TUScope);
  return R.empty() ? nullptr : R.getRepresentativeDecl();
}

// ---- 5.8: layout of one record --------------------------------------------

struct Region {
  uint64_t Bit, Bits;
  std::string What;
};

static std::string where(uint64_t Bit) {
  std::string S = std::to_string(Bit / 8);
  if (Bit % 8)
    S += "." + std::to_string(Bit % 8);
  return S;
}

static void printLayout(ASTContext &C, const CXXRecordDecl *RD) {
  RD = RD->getDefinition();
  const ASTRecordLayout &L = C.getASTRecordLayout(RD);
  llvm::outs() << (RD->isUnion() ? "union " : "struct ") << str(C, C.getCanonicalTagType(RD)) << ": size " << L.getSize().getQuantity()
               << ", align " << L.getAlignment().getQuantity() << ", data size " << L.getDataSize().getQuantity()
               << ", non-virtual size " << L.getNonVirtualSize().getQuantity() << "\n";

  std::vector<Region> Regions;
  if (RD->isDynamicClass() && L.hasOwnVFPtr())
    Regions.push_back({0, C.getTypeSize(C.VoidPtrTy), "<vptr>"});
  for (const CXXBaseSpecifier &B : RD->bases()) {
    const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
    // A base's data size, not its size: the derived class may reuse its tail padding.
    // A virtual base lives wherever the most-derived class puts it.
    CharUnits BOff = B.isVirtual() ? L.getVBaseClassOffset(BRD) : L.getBaseClassOffset(BRD);
    Regions.push_back({(uint64_t)BOff.getQuantity() * 8, (uint64_t)C.getASTRecordLayout(BRD).getDataSize().getQuantity() * 8,
                       std::string(B.isVirtual() ? "virtual base " : "base ") + BRD->getNameAsString()});
  }
  unsigned I = 0;
  for (const FieldDecl *F : RD->fields()) {
    uint64_t Bits = F->isBitField() ? F->getBitWidthValue() : C.getTypeSize(F->getType());
    std::string What = F->getNameAsString() + " : " + str(C, F->getType());
    if (F->isBitField())
      What += " : " + std::to_string(Bits);
    Regions.push_back({L.getFieldOffset(I++), Bits, What});
  }
  std::stable_sort(Regions.begin(), Regions.end(), [](const Region &A, const Region &B) { return A.Bit < B.Bit; });

  llvm::outs() << "  offset  bits  what\n";
  uint64_t Cursor = 0;
  auto row = [&](uint64_t Bit, uint64_t Bits, const std::string &What) {
    llvm::outs() << llvm::format("  %6s  %4llu  ", where(Bit).c_str(), (unsigned long long)Bits) << What << "\n";
  };
  for (const Region &R : Regions) {
    if (R.Bit > Cursor)
      row(Cursor, R.Bit - Cursor, "<padding>");
    row(R.Bit, R.Bits, R.What);
    Cursor = std::max(Cursor, R.Bit + R.Bits); // unions overlap: keep the furthest end
  }
  uint64_t End = (uint64_t)L.getSize().getQuantity() * 8;
  if (Cursor < End)
    row(Cursor, End - Cursor, "<tail padding>");
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_layout: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 3) {
    llvm::errs() << "usage: p05_layout setup.repl types|layout|template name...\n";
    return 2;
  }
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0])))
    ExitOnErr(I->ParseAndExecute(Code));
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();
  std::string Mode = A.Positional[1];

  for (size_t K = 2; K < A.Positional.size(); ++K) {
    NamedDecl *ND = lookup(S, A.Positional[K]);
    if (!ND) {
      llvm::outs() << A.Positional[K] << ": not found\n";
      continue;
    }
    if (Mode == "types") {
      auto *VD = llvm::dyn_cast<VarDecl>(ND);
      if (!VD) {
        llvm::outs() << A.Positional[K] << ": not a variable\n";
        continue;
      }
      QualType T = VD->getType();
      llvm::outs() << A.Positional[K] << ":\n";
      llvm::outs() << "  as written          " << str(C, T) << "\n";
      llvm::outs() << "  one step desugared  " << str(C, T.getSingleStepDesugaredType(C)) << "\n";
      llvm::outs() << "  desugared           " << str(C, T.getDesugaredType(C)) << "\n";
      llvm::outs() << "  canonical           " << str(C, T.getCanonicalType()) << "\n";
      llvm::outs() << "  class of the type   " << T->getTypeClassName() << " -> " << T.getCanonicalType()->getTypeClassName() << "\n";
    } else if (Mode == "layout") {
      auto *RD = llvm::dyn_cast<CXXRecordDecl>(ND);
      if (!RD || !RD->getDefinition()) {
        llvm::outs() << A.Positional[K] << ": not a complete class\n";
        continue;
      }
      printLayout(C, RD);
    } else if (Mode == "template") {
      auto *CT = llvm::dyn_cast<ClassTemplateDecl>(ND);
      if (!CT) {
        llvm::outs() << A.Positional[K] << ": not a class template\n";
        continue;
      }
      std::vector<ClassTemplateSpecializationDecl *> Specs(CT->specializations().begin(), CT->specializations().end());
      std::sort(Specs.begin(), Specs.end(), [&](auto *X, auto *Y) { return str(C, C.getCanonicalTagType(X)) < str(C, C.getCanonicalTagType(Y)); });
      for (ClassTemplateSpecializationDecl *Spec : Specs)
        if (Spec->isCompleteDefinition())
          printLayout(C, Spec);
    }
  }
  return 0;
}
