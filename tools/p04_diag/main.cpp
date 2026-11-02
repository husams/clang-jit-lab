// p04_diag -- errors and diagnostics from the API (Section 4.4): an
// llvm::Error per failed input, and a custom DiagnosticConsumer that receives
// every diagnostic as structured data instead of printed text.
//
// Usage: build/bin/p04_diag inputs.repl [-- compiler flags]
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

using namespace clang;

namespace {
// Collects diagnostics into plain strings. The interpreter owns the
// CompilerInstance (and so the DiagnosticsEngine), but not this consumer.
class CollectingConsumer : public DiagnosticConsumer {
public:
  std::vector<std::string> Seen;

  void HandleDiagnostic(DiagnosticsEngine::Level Level, const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info); // keeps NumErrors / NumWarnings
    const char *Name = "other";
    switch (Level) {
    case DiagnosticsEngine::Note:    Name = "note"; break;
    case DiagnosticsEngine::Remark:  Name = "remark"; break;
    case DiagnosticsEngine::Warning: Name = "warning"; break;
    case DiagnosticsEngine::Error:   Name = "error"; break;
    case DiagnosticsEngine::Fatal:   Name = "fatal"; break;
    default: break;
    }
    llvm::SmallString<256> Msg;
    Info.FormatDiagnostic(Msg);
    std::string Where = "<no location>";
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      PresumedLoc P = Info.getSourceManager().getPresumedLoc(Info.getLocation());
      if (P.isValid())
        Where = (llvm::Twine(P.getFilename()) + ":" + llvm::Twine(P.getLine()) + ":" +
                 llvm::Twine(P.getColumn())).str();
    }
    Seen.push_back(std::string(Name) + " | " + Where + " | " + Msg.str().str());
  }
};
} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_diag: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.empty()) {
    llvm::errs() << "usage: p04_diag inputs.repl [-- compiler flags]\n";
    return 2;
  }

  CollectingConsumer Consumer;
  std::unique_ptr<CompilerInstance> CI = ExitOnErr(repllab::makeCompiler(A.Compiler));
  // Before Interpreter::create(): the prelude is parsed inside create().
  CI->getDiagnostics().setClient(&Consumer, /*ShouldOwnClient=*/false);
  std::unique_ptr<Interpreter> Interp = ExitOnErr(Interpreter::create(std::move(CI)));
  llvm::outs() << "after create(): " << Consumer.Seen.size() << " diagnostics\n";

  unsigned N = 0, Failed = 0;
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0]))) {
    ++N;
    Consumer.Seen.clear();
    llvm::outs() << "[" << N << "] " << Code << "\n";
    std::string Status = "ok";
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      Status = "failed: " + llvm::toString(std::move(E)); // consumes the Error
      ++Failed;
    }
    llvm::outs() << "    -> " << Status << "\n";
    for (const std::string &D : Consumer.Seen)
      llvm::outs() << "    " << D << "\n";
  }
  llvm::outs() << "inputs=" << N << " failed=" << Failed << "\n";
  return 0;
}
