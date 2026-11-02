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

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::list<std::string> InputsOption(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("C++ input to compile; repeat to submit more inputs"),
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
  std::setvbuf(stdout, nullptr, _IONBF,
               0); // keep JIT printf and llvm::outs() in order
  llvm::outs().SetUnbuffered();

  // 1. Builder: turns driver-style arguments into a CompilerInstance.
  std::vector<std::string> All;
  All.insert(All.end(), CompilerFlags.begin(), CompilerFlags.end());
  std::vector<const char *> Argv;
  for (const std::string &S : All) {
    Argv.push_back(S.c_str());
  }
  clang::IncrementalCompilerBuilder Builder;
  // Configure the embedded compiler, independently of the host build flags.
  // These argument strings must remain alive until CreateCpp() finishes.
  Builder.SetCompilerArgs(Argv);
  // CreateCpp returns an owned CompilerInstance or an error inside Expected.
  auto CompilerOrError = Builder.CreateCpp();
  // Extract the failed Expected with takeError(); logging consumes that Error.
  if (!CompilerOrError) {
    llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<clang::CompilerInstance> CI = std::move(*CompilerOrError);
  // An embedding host loads requested frontend plugins explicitly, as the
  // clang-repl driver does; the builder alone does not load their libraries.
  // Load requested plugin libraries before the frontend starts parsing the
  // session.
  CI->LoadRequestedPlugins();
  if (CI->getDiagnostics().hasErrorOccurred()) {
    return 1;
  }

  // 2. Interpreter: takes ownership of the CompilerInstance.
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = clang::Interpreter::create(std::move(CI));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<clang::Interpreter> Interp = std::move(*InterpreterOrError);

  std::vector<std::string> Inputs(InputsOption.begin(), InputsOption.end());
  if (Inputs.empty()) {
    Inputs = {
        R"cpp(
int answer = 41;
)cpp",
        R"cpp(
#include <cstdio>
printf("statement: answer + 1 = %d\n", answer + 1);
)cpp",
        R"cpp(
answer * 2
)cpp",
    };
  }

  // 3. Feed it code. Each call is one input, one PTU.
  for (const std::string &Code : Inputs) {
    llvm::outs() << "input: "
                 << llvm::StringRef(Code).trim().take_until(
                        [](char C) { return C == '\n'; })
                 << "\n";
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "  error: ");
      return 1;
    }
  }
  return 0;
}
