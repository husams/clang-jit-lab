#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <llvm/ADT/SmallVector.h>
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
int counter = 41;
)cpp",
    R"cpp(
auto ratio = counter / 4.0;
)cpp",
    R"cpp(
extern "C" int c_add(int a, int b) {
  return a + b;
}
)cpp",
    R"cpp(
int over(int x) {
  return x;
}
)cpp",
    R"cpp(
int over(double x) {
  return (int)x;
}
)cpp",
    R"cpp(
int over(int x, int y) {
  return x + y;
}
)cpp",
    R"cpp(
struct Point {
  int x;
  int y;
};
)cpp",
    R"cpp(
Point origin{3, 4};
)cpp",
    R"cpp(
enum Mode { Fast, Slow = 5 };
)cpp",
    R"cpp(
using Id = long;
)cpp",
    R"cpp(
namespace geo {
int unit = 10;
struct Box {
  int w;
  int h;
};
double area(double w, double h) {
  return w * h;
}
namespace inner {
int deep = 3;
}
} // namespace geo
)cpp",
    R"cpp(
template <typename T> T twice(T x) {
  return x + x;
}
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: lookup, qualified, value, decls"),
      llvm::cl::cat(Options), llvm::cl::init("lookup"));
  llvm::cl::list<std::string> NamesOption(
      "name", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
  llvm::cl::opt<std::string> Scope(
      "scope", llvm::cl::desc("Namespace path used by qualified lookup"),
      llvm::cl::cat(Options), llvm::cl::init("geo::inner"));
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

  std::vector<std::string> Names(NamesOption.begin(), NamesOption.end());
  if (Names.empty()) {
    Names = {"counter", "ratio", "over", "Point"};
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

  if (Mode == "lookup") {
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      // LookupResult can contain a single declaration, an overload set or no
      // match.
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      // Use the live Sema and TU scope to apply the session's C++ name-lookup
      // rules.
      S.LookupName(R, S.TUScope);
      {
        auto &&IArgument = (*I);
        auto &&SArgument = (S);
        auto &&LabelArgument = (Name);
        auto &&RArgument = (R);
        llvm::outs() << LabelArgument << ": ";
        if (RArgument.empty()) {
          llvm::outs() << "not found\n";
        } else {
          // An overload name is a set of candidates, not an already selected
          // callable.
          if (RArgument.isOverloadedResult()) {
            llvm::outs() << "overload set of "
                         << RArgument.end() - RArgument.begin() << "\n";
          }
          for (clang::NamedDecl *Declaration : RArgument) {
            llvm::outs() << Declaration->getDeclKindName() << "Decl "
                         << Declaration->getQualifiedNameAsString();
            if (auto *Value = llvm::dyn_cast<clang::ValueDecl>(Declaration)) {
              llvm::outs() << R"cpp(: type ')cpp"
                           << Value->getType().getAsString(
                                  IArgument.getASTContext().getPrintingPolicy())
                           << R"cpp(')cpp";
            }
            llvm::outs() << "\n";
          }
        }
      }
    }
  } else if (Mode == "qualified") {
    // Walk a scope path such as geo::inner, one qualified lookup per step,
    // starting at the translation unit.
    DeclContext *DC = C.getTranslationUnitDecl();
    llvm::SmallVector<llvm::StringRef, 4> Path;
    llvm::StringRef(Scope).split(Path, "::");
    for (llvm::StringRef Step : Path) {
      LookupResult R(S, DeclarationName(&C.Idents.get(Step)), SourceLocation(),
                     Sema::LookupNamespaceName);
      // Resolve one name within this DeclContext; descend through namespaces
      // one scope at a time.
      S.LookupQualifiedName(R, DC);
      auto *NS =
          R.empty() ? nullptr : llvm::dyn_cast<NamespaceDecl>(R.getFoundDecl());
      if (!NS) {
        llvm::outs() << "no namespace " << Step << "\n";
        return 1;
      }
      DC = NS;
    }
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      S.LookupQualifiedName(R, DC);
      {
        auto &&IArgument2 = (*I);
        auto &&SArgument2 = (S);
        auto &&LabelArgument2 = (Scope + "::" + Name);
        auto &&RArgument2 = (R);
        llvm::outs() << LabelArgument2 << ": ";
        if (RArgument2.empty()) {
          llvm::outs() << "not found\n";
        } else {
          if (RArgument2.isOverloadedResult()) {
            llvm::outs() << "overload set of "
                         << RArgument2.end() - RArgument2.begin() << "\n";
          }
          for (clang::NamedDecl *Declaration : RArgument2) {
            llvm::outs() << Declaration->getDeclKindName() << "Decl "
                         << Declaration->getQualifiedNameAsString();
            if (auto *Value = llvm::dyn_cast<clang::ValueDecl>(Declaration)) {
              llvm::outs()
                  << R"cpp(: type ')cpp"
                  << Value->getType().getAsString(
                         IArgument2.getASTContext().getPrintingPolicy())
                  << R"cpp(')cpp";
            }
            llvm::outs() << "\n";
          }
        }
      }
    }
  } else if (Mode == "value") {
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      S.LookupName(R, S.TUScope);
      auto *VD =
          R.empty() ? nullptr : llvm::dyn_cast<VarDecl>(R.getFoundDecl());
      if (!VD) {
        llvm::outs() << Name << ": not a variable\n";
        continue;
      }
      // The address of the variable's storage in the JIT: no value printing
      // involved.
      // Resolve a runtime address; lookup may compile/link a module on demand.
      // The session owns code/storage; function-pointer types must match the
      // ABI.
      auto AddressOrError = I->getSymbolAddress(GlobalDecl(VD));
      if (!AddressOrError) {
        llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *P = (*AddressOrError).toPtr<void *>();
      QualType T = VD->getType();
      std::string AddressText;
      llvm::raw_string_ostream OutputForAddressText(AddressText);
      OutputForAddressText << llvm::format(
          "0x%llxUL",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>((P))));
      llvm::outs() << Name << ": '" << (T.getAsString(C.getPrintingPolicy()))
                   << "' at " << (AddressText) << ", "
                   << C.getTypeSizeInChars(T).getQuantity() << " bytes";
      if (const auto *BT = T->getAs<BuiltinType>()) {
        if (BT->getKind() == BuiltinType::Int) {
          llvm::outs() << ", value " << *static_cast<int *>(P);
        } else if (BT->getKind() == BuiltinType::Double) {
          llvm::outs() << ", value "
                       << llvm::format("%g", *static_cast<double *>(P));
        }
      }
      llvm::outs() << "\n";
      if (Name ==
          "counter") { // write through the address; the interpreter sees it
        *static_cast<int *>(P) = 1000;
        if (llvm::Error Error = I->ParseAndExecute(R"cpp(
counter
)cpp")) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
      }
    }
  } else if (Mode == "decls") {
    SourceManager &SM = C.getSourceManager();
    TranslationUnitDecl *TU = C.getTranslationUnitDecl();
    llvm::outs() << "getTranslationUnitDecl()->decls(): "
                 << std::distance(TU->decls_begin(), TU->decls_end())
                 << " declaration(s): only the newest input\n";

    // Every input is its own TranslationUnitDecl, chained as redeclarations,
    // newest first. input_line_0 is the interpreter's own runtime prelude.
    std::vector<TranslationUnitDecl *> Parts(TU->redecls().begin(),
                                             TU->redecls().end());
    llvm::outs() << "redecls(): " << Parts.size() << " parts\n";
    std::function<void(Decl *)> show = [&](Decl *D) {
      llvm::StringRef File =
          SM.getFilename(SM.getSpellingLoc(D->getLocation()));
      if (D->isImplicit() || !File.starts_with("input_line_") ||
          File == "input_line_0") {
        return;
      }
      if (auto *LS = llvm::dyn_cast<LinkageSpecDecl>(D)) { // extern "C" { ... }
        for (Decl *Inner : LS->decls()) {
          show(Inner);
        }
      } else if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        std::string DeclarationText;

        ASTContext &DeclarationContext = (*I).getASTContext();
        llvm::raw_string_ostream DeclarationOutput(DeclarationText);
        DeclarationOutput << ND->getDeclKindName() << "Decl";
        if (auto *VD = llvm::dyn_cast<VarDecl>((ND))) {
          DeclarationOutput
              << ": type '"
              << ((VD->getType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
          if (VD->hasInit()) {
            std::string InitializerText;
            llvm::raw_string_ostream InitializerOutput(InitializerText);
            (VD->getInit())
                ->printPretty(InitializerOutput, nullptr,
                              (DeclarationContext).getPrintingPolicy());
            DeclarationOutput << ", init '" << InitializerText << "'";
          }
        } else if (auto *FD = llvm::dyn_cast<FunctionDecl>((ND))) {
          DeclarationOutput
              << ": type '"
              << ((FD->getType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
          if (FD->isExternC()) {
            DeclarationOutput << ", extern \"C\"";
          }
        } else if (auto *TD = llvm::dyn_cast<TypedefNameDecl>((ND))) {
          DeclarationOutput
              << ": '" << ND->getName() << "' names '"
              << ((TD->getUnderlyingType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "', canonical '"
              << ((TD->getUnderlyingType().getCanonicalType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
        } else if (auto *RD = llvm::dyn_cast<CXXRecordDecl>((ND))) {
          DeclarationOutput
              << ": " << (RD->hasDefinition() ? "defined" : "declared only")
              << ", " << std::distance(RD->field_begin(), RD->field_end())
              << " fields";
        } else if (auto *ED = llvm::dyn_cast<EnumDecl>((ND))) {
          DeclarationOutput
              << ": underlying '"
              << ((ED->getIntegerType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
        } else if (auto *EC = llvm::dyn_cast<EnumConstantDecl>((ND))) {
          DeclarationOutput << ": value " << EC->getInitVal() << " of enum '"
                            << EC->getType().getAsString() << "'";
        } else if (auto *FT = llvm::dyn_cast<FunctionTemplateDecl>((ND))) {
          DeclarationOutput << ": " << FT->getTemplateParameters()->size()
                            << " template parameter(s), "
                            << std::distance(FT->specializations().begin(),
                                             FT->specializations().end())
                            << " specialization(s)";
        } else if (auto *NS = llvm::dyn_cast<NamespaceDecl>((ND))) {
          DeclarationOutput << ": '" << NS->getQualifiedNameAsString() << "'";
        }

        llvm::outs() << "  " << File << "  " << ND->getNameAsString() << "  "
                     << DeclarationText << "\n";
      }
    };
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It) {
      for (Decl *D : (*It)->decls()) {
        show(D);
      }
    }

    // The PTU that Parse() returns is the newest part.
    // Build the input's AST and IR without running its initializers.
    auto ParsedInputOrError = I->Parse(R"cpp(
int late = 1;
)cpp");
    if (!ParsedInputOrError) {
      llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    PartialTranslationUnit &PTU = (*ParsedInputOrError);
    llvm::outs() << "Parse(): PTU.TUPart is the new TU: "
                 << (PTU.TUPart == C.getTranslationUnitDecl()) << ", with "
                 << std::distance(PTU.TUPart->decls_begin(),
                                  PTU.TUPart->decls_end())
                 << " declaration(s)\n";
  }
  return 0;
}
