#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <utility>
#include <vector>

int main(int argc, const char **argv) {
  // Register LLVM's native target and assembly support for generated code.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  std::vector<const char *> DriverArgs;
  for (int N = 1; N < argc; ++N) {
    if (llvm::StringRef(argv[N]) != "--") {
      DriverArgs.push_back(argv[N]);
    }
  }
  clang::IncrementalCompilerBuilder Builder;
  // Configure the embedded compiler, independently of the host build flags.
  // These argument strings must remain alive until CreateCpp() finishes.
  Builder.SetCompilerArgs(DriverArgs);
  // CreateCpp returns an owned CompilerInstance or an error inside Expected.
  auto CompilerOrError = Builder.CreateCpp();
  // Extract the failed Expected with takeError(); logging consumes that Error.
  if (!CompilerOrError) {
    llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Compiler = std::move(*CompilerOrError);
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = clang::Interpreter::create(std::move(Compiler));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Engine = std::move(*InterpreterOrError);

  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = Engine->ParseAndExecute(
          "extern \"C\" int run(int x) { return x * 7; }")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  auto AddressOrError = Engine->getSymbolAddress("run");
  if (!AddressOrError) {
    llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Address = std::move(*AddressOrError);
  // toPtr is a cast: the definition must match this int(int) signature.
  auto Run = Address.toPtr<int (*)(int)>();
  llvm::outs() << "run(6) = " << Run(6) << "\n";

  // Pass &Result to capture the expression instead of printing it.
  // Destroy Result before Engine, which owns its type/runtime support.
  clang::Value Result;
  if (llvm::Error Error = Engine->ParseAndExecute("run(5)", &Result)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Read this known int result only after checking validity and void.
  if (Result.isValid() && !Result.isVoid()) {
    llvm::outs() << "captured = " << Result.convertTo<int>() << "\n";
  }
  return 0;
}
