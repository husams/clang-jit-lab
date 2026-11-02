#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
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
  llvm::cl::OptionCategory Options("JIT example options");

  llvm::cl::list<std::string> CompilerFlags(
      llvm::cl::Positional, llvm::cl::ZeroOrMore,
      llvm::cl::desc("<compiler flags after -->"), llvm::cl::cat(Options));
  llvm::cl::HideUnrelatedOptions(Options);
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "Clang JIT API example\n",
                                         &llvm::errs())) {
    return 2;
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  std::vector<const char *> DriverArgs;
  for (const std::string &Flag : CompilerFlags) {
    DriverArgs.push_back(Flag.c_str());
  }
  clang::IncrementalCompilerBuilder Builder;
  Builder.SetCompilerArgs(DriverArgs);
  auto CompilerOrError = Builder.CreateCpp();
  if (!CompilerOrError) {
    llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Compiler = std::move(*CompilerOrError);
  auto InterpreterOrError = clang::Interpreter::create(std::move(Compiler));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Engine = std::move(*InterpreterOrError);
  const char *Source = R"cpp(
extern "C" int run(int x) {
  return x * 7;
}
)cpp";
  if (llvm::Error Error = Engine->ParseAndExecute(Source)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  clang::Value Result;
  if (llvm::Error Error = Engine->ParseAndExecute(R"cpp(
run(5)
)cpp",
                                                  &Result)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (Result.isValid() && !Result.isVoid()) {
    llvm::outs() << "captured = " << Result.convertTo<int>() << "\n";
  }
  return 0;
}