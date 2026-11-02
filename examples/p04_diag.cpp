#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
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
// Collects diagnostics into plain strings. The interpreter owns the
// CompilerInstance (and so the DiagnosticsEngine), but not this consumer.
class CollectingConsumer : public DiagnosticConsumer {
public:
  std::vector<std::string> Seen;

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level,
                                         Info); // keeps NumErrors / NumWarnings
    const char *Name = "other";
    switch (Level) {
    case DiagnosticsEngine::Note:
      Name = "note";
      break;
    case DiagnosticsEngine::Remark:
      Name = "remark";
      break;
    case DiagnosticsEngine::Warning:
      Name = "warning";
      break;
    case DiagnosticsEngine::Error:
      Name = "error";
      break;
    case DiagnosticsEngine::Fatal:
      Name = "fatal";
      break;
    default:
      break;
    }
    llvm::SmallString<256> Msg;
    // FormatDiagnostic expands Clang's message arguments without printing to
    // stderr.
    Info.FormatDiagnostic(Msg);
    std::string Where = "<no location>";
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      // Resolve the diagnostic location only when a SourceManager and valid
      // location exist.
      PresumedLoc P =
          Info.getSourceManager().getPresumedLoc(Info.getLocation());
      if (P.isValid()) {
        Where = (llvm::Twine(P.getFilename()) + ":" + llvm::Twine(P.getLine()) +
                 ":" + llvm::Twine(P.getColumn()))
                    .str();
      }
    }
    Seen.push_back(std::string(Name) + " | " + Where + " | " + Msg.str().str());
  }
};
} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int good = 1;
)cpp",
    R"cpp(
int x = unknown_name;
)cpp",
    R"cpp(
int y = "text";
)cpp",
    R"cpp(
int w = (int)3.5 / 0;
)cpp",
    R"cpp(
void f() {
  int unused;
}
)cpp",
    R"cpp(
good + 1
)cpp",
    R"cpp(
struct P {
  int a;
};
P p;
p.b
)cpp",
    R"cpp(
int good = 2;
)cpp",
    R"cpp(
good + 41
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

  CollectingConsumer Consumer;
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
  std::unique_ptr<CompilerInstance> CI = std::move(*CompilerOrError);
  // Before Interpreter::create(): the prelude is parsed inside create().
  // Borrow this stack consumer rather than deleting it; install before prelude
  // diagnostics.
  CI->getDiagnostics().setClient(&Consumer, /*ShouldOwnClient=*/false);
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = Interpreter::create(std::move(CI));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> Interp = std::move(*InterpreterOrError);
  llvm::outs() << "after create(): " << Consumer.Seen.size()
               << " diagnostics\n";

  unsigned N = 0, Failed = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    Consumer.Seen.clear();
    llvm::outs() << "[" << N << "] " << Code << "\n";
    std::string Status = "ok";
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      Status = "failed: " + llvm::toString(std::move(E)); // consumes the Error
      ++Failed;
    }
    llvm::outs() << "    -> " << Status << "\n";
    for (const std::string &D : Consumer.Seen) {
      llvm::outs() << "    " << D << "\n";
    }
  }
  llvm::outs() << "inputs=" << N << " failed=" << Failed << "\n";
  return 0;
}
