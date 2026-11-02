#include <clang/AST/Decl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// ORC lists the symbols that failed to materialize in hash order; keep the
// message but drop the set so the output is the same on every run.

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
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
#include <cstdio>
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  llvm::outs() << "== 1. a statement that prints\n";
  // Build the input's AST and IR without running its initializers.
  auto ParsedInputOrError = I->Parse(R"cpp(
printf("hello from the JIT\n");
)cpp");
  if (!ParsedInputOrError) {
    llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // The PTU is interpreter-owned: inspect its AST and module before execution.
  PartialTranslationUnit &S = (*ParsedInputOrError);
  {
    auto &&WhenArgument = ("after Parse  ");
    auto &&PTUArgument = (S);

    unsigned Decls = std::distance(PTUArgument.TUPart->decls_begin(),
                                   PTUArgument.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument.TheModule) {
      llvm::outs() << " name=" << PTUArgument.TheModule->getName();
    }
    llvm::outs() << "\n";
  }
  llvm::outs() << "  (nothing has run yet)\n";
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error Error = I->Execute(S)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&WhenArgument2 = ("after Execute");
    auto &&PTUArgument2 = (S);

    unsigned Decls = std::distance(PTUArgument2.TUPart->decls_begin(),
                                   PTUArgument2.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument2 << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument2.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument2.TheModule) {
      llvm::outs() << " name=" << PTUArgument2.TheModule->getName();
    }
    llvm::outs() << "\n";
  }

  llvm::outs() << "== 2. a definition, parsed but not executed\n";
  auto ParsedInputOrError2 = I->Parse(R"cpp(
int triple(int v) {
  return 3 * v;
}
)cpp");
  if (!ParsedInputOrError2) {
    llvm::logAllUnhandledErrors(ParsedInputOrError2.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // Keep this definition unexecuted to separate AST visibility from runtime
  // availability.
  PartialTranslationUnit &F = (*ParsedInputOrError2);
  {
    auto &&WhenArgument3 = ("after Parse  ");
    auto &&PTUArgument3 = (F);

    unsigned Decls = std::distance(PTUArgument3.TUPart->decls_begin(),
                                   PTUArgument3.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument3 << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument3.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument3.TheModule) {
      llvm::outs() << " name=" << PTUArgument3.TheModule->getName();
    }
    llvm::outs() << "\n";
  }
  llvm::outs() << "  getSymbolAddress(_Z6triplei): ";
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  if (auto Addr = I->getSymbolAddress("_Z6triplei")) {
    llvm::outs() << "found\n";
  } else {
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  }
  llvm::outs() << "  a later ParseAndExecute(\"triple(14)\"):\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
triple(14)
)cpp")) {
    llvm::outs() << "  error: " << (llvm::toString((std::move(E)))) << "\n";
  }
  llvm::outs() << "  Execute the pending definition:\n";
  if (llvm::Error E = I->Execute(F)) {
    llvm::outs() << "  error: " << (llvm::toString((std::move(E)))) << "\n";
  }
  llvm::outs() << "  getSymbolAddress(_Z6triplei) now: ";
  if (auto Addr = I->getSymbolAddress("_Z6triplei")) {
    llvm::outs() << "found\n";
  } else {
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  }
  llvm::outs() << "  ParseAndExecute(\"triple(14)\") again:\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
triple(14)
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== 3. parse two inputs, then execute them in order\n";
  auto ParsedInputOrError3 = I->Parse(R"cpp(
int quad(int v) {
  return 4 * v;
}
)cpp");
  if (!ParsedInputOrError3) {
    llvm::logAllUnhandledErrors(ParsedInputOrError3.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  PartialTranslationUnit &D = (*ParsedInputOrError3);
  auto ParsedInputOrError4 = I->Parse(R"cpp(
printf("quad(5) = %d\n", quad(5));
)cpp");
  if (!ParsedInputOrError4) {
    llvm::logAllUnhandledErrors(ParsedInputOrError4.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  PartialTranslationUnit &U = (*ParsedInputOrError4);
  // Execute the dependency before its caller; parsing both does not register
  // either definition.
  if (llvm::Error Error = I->Execute(D)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->Execute(U)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  return 0;
}
