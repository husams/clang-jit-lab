#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

namespace {

// Collects diagnostics as "line:col: severity: message" strings.
class Collector : public DiagnosticConsumer {
public:
  std::vector<std::string> Messages;

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(
        Level, Info); // keeps the error/warning counts
    llvm::SmallString<256> Text;
    // Collect Clang's expanded diagnostic text; API errors alone do not contain
    // this detail.
    Info.FormatDiagnostic(Text);
    const char *Sev = Level == DiagnosticsEngine::Error     ? "error"
                      : Level == DiagnosticsEngine::Warning ? "warning"
                      : Level == DiagnosticsEngine::Note    ? "note"
                                                            : "other";
    std::string Where;
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      PresumedLoc P =
          Info.getSourceManager().getPresumedLoc(Info.getLocation());
      if (P.isValid()) {
        Where = std::to_string(P.getLine()) + ":" +
                std::to_string(P.getColumn()) + ": ";
      }
    }
    Messages.push_back(Where + Sev + ": " + Text.str().str());
  }
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <vector>
)cpp",
    R"cpp(
struct Point {
  int x, y;
};
)cpp",
    R"cpp(
int area(Point p) {
  return p.x * p.y;
}
)cpp",
    R"cpp(
int broken(Point p) {
  return p.z;
}
)cpp",
    R"cpp(
int uses_area() {
  return area(Point{2, 3});
}
)cpp",
    R"cpp(
auto v = std::vector<int>{1, 2, 3};
)cpp",
    R"cpp(
undeclared_function(1);
)cpp",
    R"cpp(
template <class T> T twice(T v) {
  return v + v;
}
)cpp",
    R"cpp(
double d = twice(1.5);
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");

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

  CompilerFlags.push_back("-fsyntax-only");

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
                                "error: ");
    return 1;
  }
  auto CI = std::move(*CompilerOrError);
  auto *Diags = new Collector;
  // The compiler owns this heap consumer and receives diagnostics from session
  // creation onward.
  CI->getDiagnostics().setClient(Diags, /*ShouldOwnClient=*/true);
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = Interpreter::create(std::move(CI));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);

  // No CodeGenerator -> no executor. The interpreter says so.
  // Syntax-only has no code generator: consume the expected failure instead of
  // assuming an executor.
  if (auto EE = I->getExecutionEngine()) {
    llvm::outs() << "execution engine: present\n";
  } else {
    llvm::outs() << "execution engine: " << llvm::toString(EE.takeError())
                 << "\n";
  }

  unsigned N = 0;
  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      ++N;
      Diags->Messages.clear();
      llvm::outs() << "[" << N << "] " << Code << "\n";
      // Type-check this input; syntax-only mode builds an AST but no LLVM
      // module.
      auto PTU = I->Parse(Code);
      if (!PTU) {
        llvm::consumeError(PTU.takeError()); // just "Parsing failed."; the
                                             // details are in Diags
        llvm::outs() << "    rejected\n";
      } else {
        // Only what the input itself wrote: an #include also puts every
        // declaration of the header into the PTU; those live in other files.
        const SourceManager &SM = I->getCompilerInstance()->getSourceManager();
        std::string Names;
        // Inspect accepted declarations directly; no generated code is required
        // to read the AST.
        for (Decl *D : PTU->TUPart->decls()) {
          // Every input is a memory buffer named input_line_<N>.
          auto Location =
              SM.getPresumedLoc(SM.getExpansionLoc(D->getLocation()));
          if (!Location.isValid() || !llvm::StringRef(Location.getFilename())
                                          .starts_with("input_line_")) {
            continue;
          }
          if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
            if (ND->isImplicit()) {
              continue;
            }
            Names += std::string(Names.empty() ? "" : ", ") +
                     D->getDeclKindName() + " " + ND->getNameAsString();
          } else if (llvm::isa<TopLevelStmtDecl>(D)) {
            Names += std::string(Names.empty() ? "" : ", ") + "(statement)";
          }
        }
        llvm::outs() << "    accepted; declares: "
                     << (Names.empty() ? "nothing" : Names)
                     << "; has IR: " << (PTU->TheModule ? "yes" : "no") << "\n";
      }
      for (const std::string &M : Diags->Messages) {
        llvm::outs() << "    " << M << "\n";
      }
    }
  }

  // ParseAndExecute still works: with no module to run, it is Parse plus
  // nothing.
  // With no generated module, ParseAndExecute finishes after compiler analysis.
  llvm::Error InputError2 = I->ParseAndExecute(R"cpp(
int tail_decl = 1;
)cpp");
  bool InputAccepted1 = !InputError2;
  if (InputError2) {
    llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                "error: ");
  }
  bool OK = InputAccepted1;
  llvm::outs() << "ParseAndExecute(\"int tail_decl = 1;\"): "
               << (OK ? "ok" : "error") << "\n";
  return 0;
}
