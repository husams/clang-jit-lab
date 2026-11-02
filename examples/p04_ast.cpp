#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int base = 40;
)cpp",
    R"cpp(
int add(int v) {
  return v + base;
}
)cpp",
    R"cpp(
base = add(2);
)cpp",
    R"cpp(
base
)cpp",
    R"cpp(
namespace lab {
struct Box {
  int w;
};
int area(Box b) {
  return b.w * b.w;
}
} // namespace lab
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::list<unsigned> DumpNumbers(
      "dump", llvm::cl::ZeroOrMore,
      llvm::cl::desc(
          "Input number whose AST should be dumped; repeat for several inputs"),
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

  std::set<unsigned> DumpInputs(DumpNumbers.begin(), DumpNumbers.end());

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

  std::vector<TranslationUnitDecl *> TUs;
  unsigned N = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    llvm::outs() << "[" << N << "] " << Code << "\n";
    // Build the input's AST and IR without running its initializers.
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::outs() << "    parse failed: " << llvm::toString(PTU.takeError())
                   << "\n";
      continue;
    }
    TUs.push_back(PTU->TUPart);
    if (PTU->TUPart->decls_empty()) {
      llvm::outs() << "    (no declarations)\n";
    }
    // Visit only this input's declarations; earlier inputs have their own TU
    // parts.
    for (Decl *D : PTU->TUPart->decls()) {
      llvm::outs() << "    " << D->getDeclKindName();
      // dyn_cast tests the AST node kind and returns null for declarations
      // without names.
      if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        llvm::outs() << " " << ND->getQualifiedNameAsString();
      }
      llvm::outs() << "\n";
      if (DumpInputs.count(N)) {
        std::string ASTDump;
        llvm::raw_string_ostream OutputForASTDump(ASTDump);
        D->dump(OutputForASTDump);
        llvm::outs() << ASTDump;
      }
    }
    // Syntax-only inputs can have no module; inspect declarations before
    // transferring IR.
    if (PTU->TheModule) {
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(*PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
    }
  }

  if (TUs.size() >= 2) {
    TranslationUnitDecl *Prev = TUs[TUs.size() - 2], *Last = TUs.back();
    llvm::outs() << "---\n";
    llvm::outs() << "last two inputs have the same TranslationUnitDecl: "
                 << (Prev == Last ? "yes" : "no") << "\n";
    llvm::outs() << "their getCanonicalDecl() are the same: "
                 << (Prev->getCanonicalDecl() == Last->getCanonicalDecl()
                         ? "yes"
                         : "no")
                 << "\n";
    llvm::outs() << "ASTContext::getTranslationUnitDecl() is the last input's: "
                 << (I->getASTContext().getTranslationUnitDecl() == Last ? "yes"
                                                                         : "no")
                 << "\n";
    llvm::outs() << "the canonical decl is one of the inputs' TUs: "
                 << (llvm::is_contained(TUs, Last->getCanonicalDecl()) ? "yes"
                                                                       : "no")
                 << "\n";
  }
  return 0;
}
