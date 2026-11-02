#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Raw addresses show up as big decimal constants (inttoptr (i64 38403440896
// ...)).

// Drop the boilerplate every module carries (target, flags, attribute groups).

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int counter = 5;
)cpp",
    R"cpp(
int bump(int by) {
  counter += by;
  return counter;
}
)cpp",
    R"cpp(
bump(10);
)cpp",
    R"cpp(
counter
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> LL("ll",
                                llvm::cl::desc("Write LLVM IR to this file"),
                                llvm::cl::cat(Options), llvm::cl::init(""));
  llvm::cl::opt<bool> Full("full", llvm::cl::desc("Print complete LLVM IR"),
                           llvm::cl::cat(Options), llvm::cl::init(false));
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

  std::unique_ptr<llvm::raw_fd_ostream> File;
  if (!LL.empty()) {
    std::error_code EC;
    File = std::make_unique<llvm::raw_fd_ostream>(LL, EC);
    if (EC) {
      llvm::errs() << "ir: cannot open " << LL << ": " << EC.message() << "\n";
      return 1;
    }
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
  unsigned N = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    // Build the input's AST and IR without running its initializers.
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::logAllUnhandledErrors(PTU.takeError(), llvm::errs(), "error: ");
      continue;
    }
    std::string IR;
    llvm::raw_string_ostream IROS(IR);
    // An llvm::Module is the generated IR, not running code. Print it before
    // Execute.
    if (PTU->TheModule) { // a syntax-only run (-fsyntax-only) generates no code
      // The stream receives textual LLVM IR; nullptr requests no annotation
      // writer.
      PTU->TheModule->print(IROS, nullptr);
    }
    if (File) {
      *File << "; ---- input " << N << ": " << Code << "\n" << IR;
    } else {
      llvm::outs() << "; ---- input " << N << ": " << Code << "\n";
      llvm::outs() << IR;
    }
    if (PTU->TheModule) {
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(*PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
    }
  }
  return 0;
}
