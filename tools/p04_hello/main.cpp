// p04_hello -- the whole embedding API in one file: builder, CompilerInstance,
// Interpreter, ParseAndExecute (Section 4.2). Section 4.3 reuses it to show
// what happens without the platform flags.
//
// Usage: build/bin/p04_hello [input ...] [-- compiler flags]
//        (no inputs: a declaration, a statement and an expression)
//
// Deliberately uses the raw API; the only repllab pieces are init() and
// platformArgs().
#include "clang/Frontend/CompilerInstance.h" // Interpreter.h only forward-declares it
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "repllab.h"

int main(int argc, const char **argv) {
  // The JIT generates code for this machine: the target and its asm printer
  // must be registered before Interpreter::create (repllab::init() does it).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  std::setvbuf(stdout, nullptr, _IONBF, 0); // keep JIT printf and llvm::outs() in order
  llvm::outs().SetUnbuffered();
  llvm::ExitOnError ExitOnErr("p04_hello: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  // 1. Builder: turns driver-style arguments into a CompilerInstance.
  std::vector<std::string> All = repllab::platformArgs();
  All.insert(All.end(), A.Compiler.begin(), A.Compiler.end());
  std::vector<const char *> Argv;
  for (const std::string &S : All)
    Argv.push_back(S.c_str());
  clang::IncrementalCompilerBuilder Builder;
  Builder.SetCompilerArgs(Argv);
  std::unique_ptr<clang::CompilerInstance> CI = ExitOnErr(Builder.CreateCpp());
  // An embedding host loads requested frontend plugins explicitly, as the
  // clang-repl driver does; the builder alone does not load their libraries.
  CI->LoadRequestedPlugins();
  if (CI->getDiagnostics().hasErrorOccurred())
    return 1;

  // 2. Interpreter: takes ownership of the CompilerInstance.
  std::unique_ptr<clang::Interpreter> Interp = ExitOnErr(clang::Interpreter::create(std::move(CI)));

  std::vector<std::string> Inputs = A.Positional;
  if (Inputs.empty())
    Inputs = {"int answer = 41;",
              "#include <cstdio>\nprintf(\"statement: answer + 1 = %d\\n\", answer + 1);",
              "answer * 2"};

  // 3. Feed it code. Each call is one input, one PTU.
  for (const std::string &Code : Inputs) {
    llvm::outs() << "input: " << llvm::StringRef(Code).take_until([](char C) { return C == '\n'; }) << "\n";
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "  error: ");
      return 1;
    }
  }
  return 0;
}
