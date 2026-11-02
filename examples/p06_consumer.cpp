#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
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

// Logs what the parser produces. HandleTopLevelDecl runs once per top-level
// declaration group while an input is being parsed; HandleTranslationUnit once
// per input (the incremental parser ends every input like a tiny translation
// unit).
class LogConsumer : public ASTConsumer {
  unsigned &Input;

public:
  explicit LogConsumer(unsigned &Input) : Input(Input) {}

  // Clang calls this during parsing; a declaration group can contain several
  // declarations.
  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    for (Decl *D : DG) {
      llvm::outs() << "  [consumer] input " << Input << ": "
                   << D->getDeclKindName();
      if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        llvm::outs() << " '" << ND->getQualifiedNameAsString() << "'";
      }
      llvm::outs() << "\n";
    }
    return true;
  }

  // This callback marks one incremental input ending, rather than the whole
  // session.
  void HandleTranslationUnit(ASTContext &) override {
    llvm::outs() << "  [consumer] input " << Input
                 << ": end of translation unit\n";
  }
};

// The protected constructor is only reachable from a subclass. It takes the
// executor builder (nullptr = default) and the consumer; it does NOT parse the
// runtime prelude that Interpreter::create() adds afterwards.
class LoggingInterpreter : public Interpreter {
public:
  LoggingInterpreter(std::unique_ptr<CompilerInstance> CI, llvm::Error &Err,
                     std::unique_ptr<ASTConsumer> C)
      : Interpreter(std::move(CI), Err, /*IEB=*/nullptr, std::move(C)) {}
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int a = 41;
)cpp",
    R"cpp(
int next() {
  return ++a;
}
)cpp",
    R"cpp(
struct S {
  int x;
};
)cpp",
    R"cpp(
next();
)cpp",
    R"cpp(
a
)cpp",
    R"cpp(
__CLANG_REPL__
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

  std::vector<std::string> Inputs;
  for (const std::string &Code : SourceInputs) {
    if (!llvm::StringRef(Code).starts_with("//")) {
      Inputs.push_back(Code);
    }
  }

  // 1. The subclass. The consumer needs to know the input number, so the
  //    counter lives outside both.
  unsigned Input = 0;
  llvm::outs()
      << "== LoggingInterpreter (protected constructor + LogConsumer)\n";
  {
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
    // Extract the failed Expected with takeError(); logging consumes that
    // Error.
    if (!CompilerOrError) {
      llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    auto CI = std::move(*CompilerOrError);
    llvm::Error Err = llvm::Error::success();
    LoggingInterpreter I(std::move(CI), Err,
                         std::make_unique<LogConsumer>(Input));
    if (llvm::Error Error = std::move(Err)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }

    for (const std::string &Code : Inputs) {
      ++Input;
      llvm::outs() << "input " << Input << ": " << Code << "\n";
      Value V;
      // Compile this input, then submit its module and run its initializers.
      if (llvm::Error Error = I.ParseAndExecute(Code, &V)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      if (V.isValid()) {
        std::string ValueText;
        if (!V.isValid()) {
          ValueText = "<invalid Value>\n";
        } else {
          llvm::raw_string_ostream OutputForValueText(ValueText);
          V.print(OutputForValueText);
        }
        llvm::outs() << "  value: " << ValueText;
      }
    }

    unsigned Undone = 0;
    while (
        true) { // Undo returns an Error: success means "undid one more input"
      // Remove recent accepted inputs and their symbols; earlier writes and I/O
      // remain.
      if (llvm::Error E = I.Undo()) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "undo: " << Undone
                 << " inputs undone, then \"No input left to undo\"\n";
  }

  // 2. The stock interpreter, same inputs, no consumer.
  llvm::outs()
      << "\n== Interpreter::create() (runtime prelude + value printing)\n";
  {
    std::vector<const char *> CompilerArguments2;
    for (const std::string &Flag : CompilerFlags) {
      CompilerArguments2.push_back(Flag.c_str());
    }
    clang::IncrementalCompilerBuilder CompilerBuilder2;
    CompilerBuilder2.SetCompilerArgs(CompilerArguments2);
    auto CompilerOrError2 = CompilerBuilder2.CreateCpp();
    if (!CompilerOrError2) {
      llvm::logAllUnhandledErrors(CompilerOrError2.takeError(), llvm::errs(),
                                  "compiler: ");
      return 1;
    }

    // Give the session the compiler and prepare its runtime prelude.
    auto InterpreterOrError =
        clang::Interpreter::create(std::move(*CompilerOrError2), nullptr);
    if (!InterpreterOrError) {
      llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    auto I = std::move(*InterpreterOrError);
    unsigned N = 0;
    for (const std::string &Code : Inputs) {
      ++N;
      llvm::outs() << "input " << N << ": " << Code << "\n";
      if (llvm::Error Error = I->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
    }
    unsigned Undone = 0;
    while (true) {
      if (llvm::Error E = I->Undo()) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "undo: " << Undone
                 << " inputs undone, then \"No input left to undo\"\n";
  }
  return 0;
}
