// p06_check -- a syntax-only interpreter used as a C++ snippet checker
// (Section 6.8). The compiler runs with -fsyntax-only, so the interpreter has
// no code generator and therefore no executor: nothing is ever run, but every
// input is fully parsed and type-checked, and the AST of the session grows as
// usual.
//
// For each input the tool reports the diagnostics (collected through its own
// DiagnosticConsumer instead of being printed) and the names the input
// declared (read from the PTU's TranslationUnitDecl).
//
// Usage: build/bin/p06_check inputs.repl [-- compiler flags]
//        (the flags default to -fsyntax-only; add -Xclang -ast-dump for the
//         AST-dump variant)
#include "repllab.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Interpreter/PartialTranslationUnit.h"

using namespace clang;

namespace {

// Collects diagnostics as "line:col: severity: message" strings.
class Collector : public DiagnosticConsumer {
public:
  std::vector<std::string> Messages;

  void HandleDiagnostic(DiagnosticsEngine::Level Level, const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info); // keeps the error/warning counts
    llvm::SmallString<256> Text;
    Info.FormatDiagnostic(Text);
    const char *Sev = Level == DiagnosticsEngine::Error     ? "error"
                      : Level == DiagnosticsEngine::Warning ? "warning"
                      : Level == DiagnosticsEngine::Note    ? "note"
                                                            : "other";
    std::string Where;
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      PresumedLoc P = Info.getSourceManager().getPresumedLoc(Info.getLocation());
      if (P.isValid())
        Where = std::to_string(P.getLine()) + ":" + std::to_string(P.getColumn()) + ": ";
    }
    Messages.push_back(Where + Sev + ": " + Text.str().str());
  }
};

} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_check: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Compiler.empty())
    A.Compiler = {"-fsyntax-only"};

  auto CI = ExitOnErr(repllab::makeCompiler(A.Compiler));
  auto *Diags = new Collector;
  CI->getDiagnostics().setClient(Diags, /*ShouldOwnClient=*/true);
  std::unique_ptr<Interpreter> I = ExitOnErr(Interpreter::create(std::move(CI)));

  // No CodeGenerator -> no executor. The interpreter says so.
  if (auto EE = I->getExecutionEngine())
    llvm::outs() << "execution engine: present\n";
  else
    llvm::outs() << "execution engine: " << llvm::toString(EE.takeError()) << "\n";

  unsigned N = 0;
  for (const std::string &File : A.Positional) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      ++N;
      Diags->Messages.clear();
      llvm::outs() << "[" << N << "] " << Code << "\n";
      auto PTU = I->Parse(Code);
      if (!PTU) {
        llvm::consumeError(PTU.takeError()); // just "Parsing failed."; the details are in Diags
        llvm::outs() << "    rejected\n";
      } else {
        // Only what the input itself wrote: an #include also puts every
        // declaration of the header into the PTU; those live in other files.
        const SourceManager &SM = I->getCompilerInstance()->getSourceManager();
        std::string Names;
        for (Decl *D : PTU->TUPart->decls()) {
          // Every input is a memory buffer named input_line_<N>.
          if (!llvm::StringRef(SM.getPresumedLoc(SM.getExpansionLoc(D->getLocation())).getFilename())
                   .starts_with("input_line_"))
            continue;
          if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
            if (ND->isImplicit())
              continue;
            Names += std::string(Names.empty() ? "" : ", ") + D->getDeclKindName() + " " + ND->getNameAsString();
          } else if (llvm::isa<TopLevelStmtDecl>(D)) {
            Names += std::string(Names.empty() ? "" : ", ") + "(statement)";
          }
        }
        llvm::outs() << "    accepted; declares: " << (Names.empty() ? "nothing" : Names)
                     << "; has IR: " << (PTU->TheModule ? "yes" : "no") << "\n";
      }
      for (const std::string &M : Diags->Messages)
        llvm::outs() << "    " << M << "\n";
    }
  }

  // ParseAndExecute still works: with no module to run, it is Parse plus nothing.
  bool OK = repllab::run(*I, "int tail_decl = 1;");
  llvm::outs() << "ParseAndExecute(\"int tail_decl = 1;\"): " << (OK ? "ok" : "error") << "\n";
  return 0;
}
