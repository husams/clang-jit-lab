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

  // An llvm::Error must be consumed exactly once; destroying an unchecked one
  // aborts the process in assertion builds. Each style below consumes it.
  llvm::outs() << "-- toString\n";
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int a = ;
)cpp")) {
    // toString consumes the Error and returns its message; Clang diagnostics
    // are separate.
    llvm::outs() << "message: " << llvm::toString(std::move(E)) << "\n";
  }

  llvm::outs() << "-- handleAllErrors\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int b = nope;
)cpp")) {
    // Dispatch every contained error to a handler, consuming the checked Error.
    llvm::handleAllErrors(std::move(E), [](const llvm::ErrorInfoBase &EIB) {
      llvm::outs() << "message: " << EIB.message() << "\n";
    });
  }

  llvm::outs() << "-- logAllUnhandledErrors\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int c = 'x
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "error: ");
  }

  llvm::outs() << "-- Expected from Parse\n";
  // Build the input's AST and IR without running its initializers.
  llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(R"cpp(
int d = nope;
)cpp");
  llvm::outs() << "Parse succeeded: " << (PTU ? "yes" : "no") << "\n";
  if (!PTU) {
    llvm::outs() << "message: " << llvm::toString(PTU.takeError()) << "\n";
  }

  llvm::outs() << "-- Undo\n";
  llvm::outs() << "Undo(1) on a fresh interpreter: ";
  // Remove recent accepted inputs and their symbols; earlier writes and I/O
  // remain.
  llvm::outs() << llvm::toString(I->Undo()) << "\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int e = 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f = 2;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  llvm::outs() << "Undo(5) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(5)) << "\n";
  llvm::outs() << "Undo(2) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(2)) << "\n";
  llvm::outs() << "(llvm::toString of a success is the empty string)\n";

  llvm::outs() << "-- the interpreter still works\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int g = 7;
g
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  return 0;
}
