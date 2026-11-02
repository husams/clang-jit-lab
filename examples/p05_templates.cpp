#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
template <typename T> T scale(T x) {
  return x * 3;
}
)cpp",
    R"cpp(
template <typename T> struct Box {
  T value;
  char tag;
  T other;
};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: address, explicit, class"),
      llvm::cl::cat(Options), llvm::cl::init("address"));
  llvm::cl::list<std::string> TypesOption(
      "type", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
  llvm::cl::list<std::string> CompilerFlags(
      llvm::cl::Positional, llvm::cl::ZeroOrMore,
      llvm::cl::desc("<compiler flags after -->"), llvm::cl::cat(Options));
  llvm::cl::HideUnrelatedOptions(Options);
  // Fill the registered options; malformed host arguments stop before compiler
  // setup.
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "Clang JIT API example\n",
                                         &llvm::errs())) {
    return 2;
  }

  // Register LLVM's native target and assembly support for generated code.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  llvm::outs().SetUnbuffered();

  std::vector<std::string> Types(TypesOption.begin(), TypesOption.end());
  if (Types.empty()) {
    Types = {"int", "double"};
  }
  std::vector<const char *> CompilerArguments;
  for (const std::string &Flag : CompilerFlags) {
    CompilerArguments.push_back(Flag.c_str());
  }
  clang::IncrementalCompilerBuilder CompilerBuilder;
  // Configure the embedded compiler, independently of the host build flags.
  // These argument strings must remain alive until CreateCpp() finishes.
  CompilerBuilder.SetCompilerArgs(CompilerArguments);
  // CreateCpp returns an owned CompilerInstance or an error inside Expected.
  auto CompilerOrError = CompilerBuilder.CreateCpp();
  // Extract the failed Expected with takeError(); logging consumes that Error.
  if (!CompilerOrError) {
    llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                "compiler: ");
    return 1;
  }

  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError =
      clang::Interpreter::create(std::move(*CompilerOrError), nullptr);
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();

  // The canonical type a type name denotes ("unsigned" and "unsigned int" are
  // the same type but not the same spelling): declare an alias and read it
  // back.
  unsigned Counter = 0;

  for (size_t K = 0; K < Types.size(); ++K) {
    const std::string &Ty = Types[K];
    llvm::outs() << "type " << Ty << ":\n";
    QualType Want;

    std::string Alias = "selected_type_" + std::to_string(Counter++);
    llvm::Error InputError2 = I->ParseAndExecute(
        R"cpp(using )cpp" + Alias + R"cpp( = )cpp" + (Ty) + R"cpp(;)cpp");
    bool InputAccepted1 = !InputError2;
    if (InputError2) {
      llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                  "error: ");
    }
    if (!InputAccepted1) {
      continue;
    }
    clang::NamedDecl *FoundDeclaration;
    clang::ASTContext &LookupContext = S.getASTContext();
    clang::LookupResult Lookup(
        (S), clang::DeclarationName(&LookupContext.Idents.get((Alias))),
        clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
    S.LookupName(Lookup, S.TUScope);
    FoundDeclaration =
        Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
    auto *TA = llvm::dyn_cast_if_present<TypedefNameDecl>(FoundDeclaration);
    Want = TA ? TA->getUnderlyingType().getCanonicalType() : QualType();

    if (Want.isNull()) {
      continue;
    }
    if (Mode == "address") {
      // Take the address of the specialization: that forces its instantiation,
      // and the initializer of the new variable leads back to the FunctionDecl.
      // Build the input's AST and IR without running its initializers.
      auto ParsedInputOrError =
          I->Parse(R"cpp(auto function_pointer_)cpp" + std::to_string(K) +
                   R"cpp( = &scale<)cpp" + Ty + R"cpp(>;)cpp");
      if (!ParsedInputOrError) {
        llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(),
                                    llvm::errs(), "error: ");
        return 1;
      }
      PartialTranslationUnit &PTU = (*ParsedInputOrError);
      // The variable initializer refers to the instantiated function; follow it
      // to FunctionDecl.
      auto *VD = llvm::cast<VarDecl>(*PTU.TUPart->decls_begin());
      auto *UO = llvm::cast<UnaryOperator>(VD->getInit());
      auto *FD = llvm::cast<FunctionDecl>(
          llvm::cast<DeclRefExpr>(UO->getSubExpr())->getDecl());
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      // Resolve a runtime address; lookup may compile/link a module on demand.
      // The session owns code/storage; function-pointer types must match the
      // ABI.
      auto AddressOrError = I->getSymbolAddress(GlobalDecl(FD));
      if (!AddressOrError) {
        llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *Addr = (*AddressOrError).toPtr<void *>();
      llvm::outs() << "  specialization "
                   << FD->getType().getAsString(C.getPrintingPolicy()) << " at "
                   << (llvm::formatv("{0:x}", (uintptr_t)Addr).str()) << "\n";
      std::string NativeCallResult;

      const auto *BT = FD->getParamDecl(0)
                           ->getType()
                           .getCanonicalType()
                           ->getAs<BuiltinType>();
      llvm::raw_string_ostream NativeCallOutput(NativeCallResult);
      // The AST identifies a runtime-selected type, but native calls still need
      // a host C++ signature.
      switch (BT ? BT->getKind() : BuiltinType::Void) {
      case BuiltinType::Int: {
        auto Function = reinterpret_cast<int (*)(int)>((Addr));
        int Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<int>(Accumulated +
                                         Function(static_cast<int>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::UInt: {
        auto Function = reinterpret_cast<unsigned (*)(unsigned)>((Addr));
        unsigned Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<unsigned>(
              Accumulated + Function(static_cast<unsigned>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Long: {
        auto Function = reinterpret_cast<long (*)(long)>((Addr));
        long Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<long>(
              Accumulated + Function(static_cast<long>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Short: {
        auto Function = reinterpret_cast<short (*)(short)>((Addr));
        short Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<short>(
              Accumulated + Function(static_cast<short>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Float: {
        auto Function = reinterpret_cast<float (*)(float)>((Addr));
        float Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<float>(
              Accumulated + Function(static_cast<float>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      case BuiltinType::Double: {
        auto Function = reinterpret_cast<double (*)(double)>((Addr));
        double Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<double>(
              Accumulated + Function(static_cast<double>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      default:
        NativeCallOutput << "(host has no case for this type)";
      }

      llvm::outs() << "  host called it 1000000 times, sum = "
                   << NativeCallResult << "\n";
    } else if (Mode == "explicit") {
      // Ask for an explicit instantiation, then find the specialization in the
      // template.
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(template )cpp" + Ty + R"cpp( scale<)cpp" + Ty +
              R"cpp(>()cpp" + Ty + R"cpp();)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      clang::NamedDecl *FoundDeclaration2;
      clang::ASTContext &LookupContext = S.getASTContext();
      clang::LookupResult Lookup(
          (S), clang::DeclarationName(&LookupContext.Idents.get(("scale"))),
          clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
      S.LookupName(Lookup, S.TUScope);
      FoundDeclaration2 =
          Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
      auto *FT =
          llvm::dyn_cast_if_present<FunctionTemplateDecl>(FoundDeclaration2);
      if (!FT) {
        llvm::outs() << "  no function template 'scale' in the session\n";
        continue;
      }
      const FunctionDecl *Found = nullptr;
      // Find the concrete specialization by canonical type, not by printed
      // spelling.
      for (const FunctionDecl *Spec : FT->specializations()) {
        if (Spec->getTemplateSpecializationArgs()
                ->get(0)
                .getAsType()
                .getCanonicalType() == Want) {
          Found = Spec;
        }
      }
      if (!Found) {
        llvm::outs() << "  no specialization found\n";
        continue;
      }
      auto AddressOrError2 = I->getSymbolAddress(GlobalDecl(Found));
      if (!AddressOrError2) {
        llvm::logAllUnhandledErrors(AddressOrError2.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *Addr = (*AddressOrError2).toPtr<void *>();
      llvm::outs() << "  specializations so far: "
                   << std::distance(FT->specializations().begin(),
                                    FT->specializations().end())
                   << ", this one "
                   << Found->getType().getAsString(C.getPrintingPolicy())
                   << "\n";
      std::string NativeCallResult2;

      const auto *BT = (Found)
                           ->getParamDecl(0)
                           ->getType()
                           .getCanonicalType()
                           ->getAs<BuiltinType>();
      llvm::raw_string_ostream NativeCallOutput(NativeCallResult2);
      switch (BT ? BT->getKind() : BuiltinType::Void) {
      case BuiltinType::Int: {
        auto Function = reinterpret_cast<int (*)(int)>((Addr));
        int Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<int>(Accumulated +
                                         Function(static_cast<int>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::UInt: {
        auto Function = reinterpret_cast<unsigned (*)(unsigned)>((Addr));
        unsigned Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<unsigned>(
              Accumulated + Function(static_cast<unsigned>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Long: {
        auto Function = reinterpret_cast<long (*)(long)>((Addr));
        long Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<long>(
              Accumulated + Function(static_cast<long>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Short: {
        auto Function = reinterpret_cast<short (*)(short)>((Addr));
        short Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<short>(
              Accumulated + Function(static_cast<short>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Float: {
        auto Function = reinterpret_cast<float (*)(float)>((Addr));
        float Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<float>(
              Accumulated + Function(static_cast<float>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      case BuiltinType::Double: {
        auto Function = reinterpret_cast<double (*)(double)>((Addr));
        double Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<double>(
              Accumulated + Function(static_cast<double>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      default:
        NativeCallOutput << "(host has no case for this type)";
      }

      llvm::outs() << "  host called it 1000000 times, sum = "
                   << NativeCallResult2 << "\n";
    } else if (Mode == "class") {
      // sizeof/alignof of the instantiation, asked of the interpreter as
      // Values...
      Value Size, Align;
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(sizeof(Box<)cpp" + Ty + R"cpp(>))cpp", &Size)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(alignof(Box<)cpp" + Ty + R"cpp(>))cpp", &Align)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      llvm::outs() << "  Value:  sizeof = " << Size.convertTo<size_t>()
                   << ", alignof = " << Align.convertTo<size_t>() << "\n";
      // ...and read back from the AST: the sizeof expression instantiated
      // Box<T>.
      clang::NamedDecl *FoundDeclaration3;
      clang::ASTContext &LookupContext = S.getASTContext();
      clang::LookupResult Lookup(
          (S), clang::DeclarationName(&LookupContext.Idents.get(("Box"))),
          clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
      S.LookupName(Lookup, S.TUScope);
      FoundDeclaration3 =
          Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
      auto *CT =
          llvm::dyn_cast_if_present<ClassTemplateDecl>(FoundDeclaration3);
      if (!CT) {
        llvm::outs() << "  no class template 'Box' in the session\n";
        continue;
      }
      for (ClassTemplateSpecializationDecl *Spec : CT->specializations()) {
        if (Spec->getTemplateArgs()[0].getAsType().getCanonicalType() != Want) {
          continue;
        }
        // sizeof/alignof forced class instantiation; the resulting record now
        // has a layout.
        const ASTRecordLayout &L = C.getASTRecordLayout(Spec);
        llvm::outs() << "  layout: size " << L.getSize().getQuantity()
                     << ", align " << L.getAlignment().getQuantity()
                     << ", fields at";
        unsigned N = 0;
        for (const FieldDecl *F : Spec->fields()) {
          llvm::outs() << " " << F->getName() << "@"
                       << L.getFieldOffset(N++) / 8;
        }
        llvm::outs() << "\n";
      }
    }
  }
  return 0;
}
