// LLVM parses host options; Clang parses the forwarded compiler flags.
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <utility>
#include <vector>

// Register typed host options before parsing argv; Clang receives its own flag
// list.
static llvm::cl::OptionCategory HostOptions("JIT host options");
// SubCommand registers a command; cl::sub below selects which options it
// accepts.
static llvm::cl::SubCommand RunCommand("run", "Compile and call a function");
static llvm::cl::SubCommand CompileCommand("compile",
                                           "Compile without executing");
static llvm::cl::opt<int>
    Input("input", llvm::cl::desc("Integer passed to the generated function"),
          llvm::cl::init(6), llvm::cl::cat(HostOptions),
          llvm::cl::sub(RunCommand));
// aliasopt shares the existing option value instead of declaring a second
// integer.
static llvm::cl::alias InputAlias("x", llvm::cl::desc("Alias for --input"),
                                  llvm::cl::aliasopt(Input),
                                  llvm::cl::cat(HostOptions));
static llvm::cl::opt<bool>
    DumpIR("dump-ir", llvm::cl::desc("Print the generated LLVM IR"),
           llvm::cl::init(false), llvm::cl::cat(HostOptions),
           llvm::cl::sub(RunCommand), llvm::cl::sub(CompileCommand));
static llvm::cl::list<std::string>
    CompilerArgs(llvm::cl::Positional, llvm::cl::ZeroOrMore,
                 llvm::cl::desc("<compiler flags after -->"),
                 llvm::cl::cat(HostOptions), llvm::cl::sub(RunCommand),
                 llvm::cl::sub(CompileCommand));

int main(int argc, const char **argv) {
  llvm::cl::HideUnrelatedOptions(HostOptions);
  llvm::cl::HideUnrelatedOptions(HostOptions, RunCommand);
  llvm::cl::HideUnrelatedOptions(HostOptions, CompileCommand);
  // Fill the registered options; malformed host arguments stop before compiler
  // setup.
  if (!llvm::cl::ParseCommandLineOptions(
          argc, argv, "Compile C++ inside a host application\n",
          &llvm::errs())) {
    return 1;
  }
  if (!RunCommand && !CompileCommand) {
    llvm::errs() << "Choose a command: run or compile; use --help for usage.\n";
    return 1;
  }

  // Register LLVM's native target and assembly support for generated code.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  std::vector<const char *> DriverArgs;
  for (const std::string &Argument : CompilerArgs) {
    DriverArgs.push_back(Argument.c_str());
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
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError =
      clang::Interpreter::create(std::move(*CompilerOrError));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Engine = std::move(*InterpreterOrError);
  // Build the input's AST and IR without running its initializers.
  auto UnitOrError = Engine->Parse(R"cpp(
#ifndef SCALE
#define SCALE 7
#endif
extern "C" int run(int x) {
  return x * SCALE;
}
)cpp");
  if (!UnitOrError) {
    llvm::logAllUnhandledErrors(UnitOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // Inspect IR before Execute transfers the module into the executor.
  if (DumpIR && UnitOrError->TheModule) {
    UnitOrError->TheModule->print(llvm::outs(), nullptr);
  }
  // A host command selects control flow; this branch never executes the parsed
  // module.
  if (CompileCommand) {
    llvm::outs() << "compiled; execution skipped\n";
    return 0;
  }
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error Error = Engine->Execute(*UnitOrError)) {
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
  auto Function = AddressOrError->toPtr<int (*)(int)>();
  llvm::outs() << "run(" << Input << ") = " << Function(Input) << "\n";
  return 0;
}
