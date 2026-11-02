// p05_templates -- pick a type at run time, have the interpreter instantiate a
// template for it, and call the result from the host (Section 5.10).
//
// Usage: build/bin/p05_templates setup.repl address|explicit|class type...
#include "../p05_anatomy/kinds.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"

using namespace clang;

// Call `Fn` a million times from the host and fold the results together, so the
// call cannot be optimised away. The host must know the C type to call it with:
// that is the one thing the AST cannot supply as a C++ type at run time.
template <typename T> static T callMany(void *Addr) {
  auto *Fn = reinterpret_cast<T (*)(T)>(Addr);
  T Acc = 0;
  for (int K = 0; K < 1000000; ++K)
    Acc = static_cast<T>(Acc + Fn(static_cast<T>(K % 7)));
  return Acc;
}

// Map the specialization's parameter type (read from the AST) to the host type.
static std::string callFromHost(const FunctionDecl *FD, void *Addr) {
  const auto *BT = FD->getParamDecl(0)->getType().getCanonicalType()->getAs<BuiltinType>();
  std::string S;
  llvm::raw_string_ostream OS(S);
  switch (BT ? BT->getKind() : BuiltinType::Void) {
  case BuiltinType::Int:
    OS << callMany<int>(Addr);
    break;
  case BuiltinType::UInt:
    OS << callMany<unsigned>(Addr);
    break;
  case BuiltinType::Long:
    OS << callMany<long>(Addr);
    break;
  case BuiltinType::Short:
    OS << callMany<short>(Addr);
    break;
  case BuiltinType::Float:
    OS << llvm::format("%.0f", (double)callMany<float>(Addr));
    break;
  case BuiltinType::Double:
    OS << llvm::format("%.0f", callMany<double>(Addr));
    break;
  default:
    OS << "(host has no case for this type)";
  }
  return S;
}

static NamedDecl *lookup(Sema &S, llvm::StringRef Name) {
  ASTContext &C = S.getASTContext();
  LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(), Sema::LookupOrdinaryName);
  S.LookupName(R, S.TUScope);
  return R.empty() ? nullptr : R.getRepresentativeDecl();
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_templates: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 3) {
    llvm::errs() << "usage: p05_templates setup.repl address|explicit|class type...\n";
    return 2;
  }
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0])))
    ExitOnErr(I->ParseAndExecute(Code));
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();
  std::string Mode = A.Positional[1];

  // The canonical type a type name denotes ("unsigned" and "unsigned int" are the
  // same type but not the same spelling): declare an alias and read it back.
  unsigned Counter = 0;
  auto canonical = [&](const std::string &Ty) -> QualType {
    std::string Alias = "p05_ty_" + std::to_string(Counter++);
    if (!repllab::run(*I, "using " + Alias + " = " + Ty + ";"))
      return QualType();
    auto *TA = llvm::dyn_cast_if_present<TypedefNameDecl>(lookup(S, Alias));
    return TA ? TA->getUnderlyingType().getCanonicalType() : QualType();
  };

  for (size_t K = 2; K < A.Positional.size(); ++K) {
    const std::string &Ty = A.Positional[K];
    llvm::outs() << "type " << Ty << ":\n";
    QualType Want = canonical(Ty);
    if (Want.isNull())
      continue;
    if (Mode == "address") {
      // Take the address of the specialization: that forces its instantiation,
      // and the initializer of the new variable leads back to the FunctionDecl.
      PartialTranslationUnit &PTU = ExitOnErr(I->Parse("auto p05_fp_" + std::to_string(K) + " = &scale<" + Ty + ">;"));
      auto *VD = llvm::cast<VarDecl>(*PTU.TUPart->decls_begin());
      auto *UO = llvm::cast<UnaryOperator>(VD->getInit());
      auto *FD = llvm::cast<FunctionDecl>(llvm::cast<DeclRefExpr>(UO->getSubExpr())->getDecl());
      ExitOnErr(I->Execute(PTU));
      void *Addr = ExitOnErr(I->getSymbolAddress(GlobalDecl(FD))).toPtr<void *>();
      llvm::outs() << "  specialization " << FD->getType().getAsString(C.getPrintingPolicy()) << " at " << repllab::maskAddrs(llvm::formatv("{0:x}", (uintptr_t)Addr).str()) << "\n";
      llvm::outs() << "  host called it 1000000 times, sum = " << callFromHost(FD, Addr) << "\n";
    } else if (Mode == "explicit") {
      // Ask for an explicit instantiation, then find the specialization in the template.
      ExitOnErr(I->ParseAndExecute("template " + Ty + " scale<" + Ty + ">(" + Ty + ");"));
      auto *FT = llvm::dyn_cast_if_present<FunctionTemplateDecl>(lookup(S, "scale"));
      if (!FT) {
        llvm::outs() << "  no function template 'scale' in the session\n";
        continue;
      }
      const FunctionDecl *Found = nullptr;
      for (const FunctionDecl *Spec : FT->specializations())
        if (Spec->getTemplateSpecializationArgs()->get(0).getAsType().getCanonicalType() == Want)
          Found = Spec;
      if (!Found) {
        llvm::outs() << "  no specialization found\n";
        continue;
      }
      void *Addr = ExitOnErr(I->getSymbolAddress(GlobalDecl(Found))).toPtr<void *>();
      llvm::outs() << "  specializations so far: " << std::distance(FT->specializations().begin(), FT->specializations().end()) << ", this one "
                   << Found->getType().getAsString(C.getPrintingPolicy()) << "\n";
      llvm::outs() << "  host called it 1000000 times, sum = " << callFromHost(Found, Addr) << "\n";
    } else if (Mode == "class") {
      // sizeof/alignof of the instantiation, asked of the interpreter as Values...
      Value Size, Align;
      ExitOnErr(I->ParseAndExecute("sizeof(Box<" + Ty + ">)", &Size));
      ExitOnErr(I->ParseAndExecute("alignof(Box<" + Ty + ">)", &Align));
      llvm::outs() << "  Value:  sizeof = " << Size.convertTo<size_t>() << ", alignof = " << Align.convertTo<size_t>() << "\n";
      // ...and read back from the AST: the sizeof expression instantiated Box<T>.
      auto *CT = llvm::dyn_cast_if_present<ClassTemplateDecl>(lookup(S, "Box"));
      if (!CT) {
        llvm::outs() << "  no class template 'Box' in the session\n";
        continue;
      }
      for (ClassTemplateSpecializationDecl *Spec : CT->specializations()) {
        if (Spec->getTemplateArgs()[0].getAsType().getCanonicalType() != Want)
          continue;
        const ASTRecordLayout &L = C.getASTRecordLayout(Spec);
        llvm::outs() << "  layout: size " << L.getSize().getQuantity() << ", align " << L.getAlignment().getQuantity() << ", fields at";
        unsigned N = 0;
        for (const FieldDecl *F : Spec->fields())
          llvm::outs() << " " << F->getName() << "@" << L.getFieldOffset(N++) / 8;
        llvm::outs() << "\n";
      }
    }
  }
  return 0;
}
