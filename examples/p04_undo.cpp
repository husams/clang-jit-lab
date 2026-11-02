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

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::list<std::string> InputsOption(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("C++ input; repeat for an incremental sequence"),
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
  llvm::InitializeNativeTargetAsmParser();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  llvm::outs().SetUnbuffered();

  if (!InputsOption.empty()) { // run the arguments as inputs, then
                               // Undo() until it fails
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
    std::unique_ptr<Interpreter> J = std::move(*InterpreterOrError);
    for (const std::string &Code : InputsOption) {
      // Compile this input, then submit its module and run its initializers.
      if (llvm::Error Error = (J)->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
    }
    unsigned Undone = 0;
    while (true) {
      // Remove recent accepted inputs and their symbols; earlier writes and I/O
      // remain.
      llvm::Error E = J->Undo();
      if (E) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "Undo() succeeded " << Undone << " times\n";
    return 0;
  }

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

  auto InterpreterOrError2 =
      clang::Interpreter::create(std::move(*CompilerOrError2), nullptr);
  if (!InterpreterOrError2) {
    llvm::logAllUnhandledErrors(InterpreterOrError2.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError2);

  llvm::outs() << "== undo a definition\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int base = 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f() {
  return base + 1;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&IArgument = (*I);
    auto &&WhenArgument = ("before Undo");
    auto &&NameArgument = ("f");
    auto &&IRNameArgument = ("_Z1fv");

    bool IsDeclared;
    // AST lookup tests compiler visibility; JIT lookup separately tests the
    // runtime symbol.
    clang::ASTContext &Context = (IArgument).getASTContext();
    clang::DeclarationName Declaration(&Context.Idents.get((NameArgument)));
    IsDeclared = !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
    llvm::outs() << "  " << WhenArgument
                 << ": declared=" << (IsDeclared ? "yes" : "no")
                 << " JIT symbol ";
    // Resolve a runtime address; lookup may compile/link a module on demand.
    // The session owns code/storage; function-pointer types must match the ABI.
    if (auto Addr = IArgument.getSymbolAddress(IRNameArgument)) {
      llvm::outs() << "found\n";
    } else {
      llvm::outs() << "missing (" << llvm::toString(Addr.takeError()) << ")\n";
    }
  }
  if (llvm::Error Error = I->Undo()) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&IArgument2 = (*I);
    auto &&WhenArgument2 = ("after Undo ");
    auto &&NameArgument2 = ("f");
    auto &&IRNameArgument2 = ("_Z1fv");

    bool IsDeclared2;
    clang::ASTContext &Context = (IArgument2).getASTContext();
    clang::DeclarationName Declaration(&Context.Idents.get((NameArgument2)));
    IsDeclared2 =
        !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
    llvm::outs() << "  " << WhenArgument2
                 << ": declared=" << (IsDeclared2 ? "yes" : "no")
                 << " JIT symbol ";
    if (auto Addr = IArgument2.getSymbolAddress(IRNameArgument2)) {
      llvm::outs() << "found\n";
    } else {
      llvm::outs() << "missing (" << llvm::toString(Addr.takeError()) << ")\n";
    }
  }
  bool IsDeclared3;
  clang::ASTContext &Context = (*I).getASTContext();
  clang::DeclarationName Declaration(&Context.Idents.get(("base")));
  IsDeclared3 = !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
  llvm::outs() << "  base is untouched: declared="
               << (IsDeclared3 ? "yes" : "no") << "\n";
  llvm::outs() << "  calling f():\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
f()
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== redefine after undo\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f() {
  return base + 100;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
f()
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== undo a global with a different type, then redefine it\n";
  if (llvm::Error Error = I->Undo(2)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  } // the new f() and the f() call that printed 101
  if (llvm::Error Error = I->Undo()) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  } // int base
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
double base = 2.5;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
base
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== the lower bound\n";
  // Inputs so far: int base (undone), double base, base  => 2 live inputs.
  llvm::outs() << "Undo(3): " << llvm::toString(I->Undo(3)) << "\n";
  llvm::outs() << "Undo(2): " << llvm::toString(I->Undo(2)) << "|\n";
  llvm::outs() << "Undo(1): " << llvm::toString(I->Undo(1)) << "\n";
  llvm::outs() << "the prelude is still there:\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
1 + 1
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  return 0;
}
