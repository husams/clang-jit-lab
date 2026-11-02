// A complete embedded Clang JIT: edit the source string, rebuild, call it.
#include <utility>
#include <vector>

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "repllab.h"

int main(int argc, const char** argv) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::ExitOnError Check("p01_engine: ");
  auto Args = repllab::splitArgs(argc, argv);

  // These strings stay alive while the builder uses their c_str() pointers.
  auto Flags = repllab::platformArgs();
  Flags.insert(Flags.end(), Args.Compiler.begin(), Args.Compiler.end());
  std::vector<const char*> DriverArgs;
  for (const auto& Flag : Flags) {
    DriverArgs.push_back(Flag.c_str());
  }
  clang::IncrementalCompilerBuilder Builder;
  Builder.SetCompilerArgs(DriverArgs);
  auto Compiler = Check(Builder.CreateCpp());
  auto Engine = Check(clang::Interpreter::create(std::move(Compiler)));

  Check(Engine->ParseAndExecute("extern \"C\" int run(int x) { return x * 7; }"));
  auto Address = Check(Engine->getSymbolAddress("run"));
  auto Run = Address.toPtr<int (*)(int)>();
  llvm::outs() << "run(6) = " << Run(6) << "\n";

  // Value must die before the interpreter that owns its type/runtime support.
  clang::Value Result;
  Check(Engine->ParseAndExecute("run(5)", &Result));
  if (Result.isValid() && !Result.isVoid()) {
    llvm::outs() << "captured = " << Result.convertTo<int>() << "\n";
  }
  return 0;
}
