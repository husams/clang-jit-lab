#include <clang/AST/Decl.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/Type.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
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
  llvm::cl::opt<std::string> SourceFile(
      "source", llvm::cl::desc("C++ source file containing run(int)"),
      llvm::cl::cat(Options), llvm::cl::Required);
  llvm::cl::list<int> InputOptions(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Integer argument for run; repeat for several calls"),
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
  std::vector<int> Inputs(InputOptions.begin(), InputOptions.end());
  if (Inputs.empty()) {
    Inputs = {0, 6, -3};
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
  auto Built = clang::Interpreter::create(std::move(*CompilerOrError), nullptr);
  if (!Built) {
    llvm::logAllUnhandledErrors(Built.takeError(), llvm::errs(), "create: ");
    return 1;
  }
  auto Engine = std::move(*Built);
  std::string SourceText;
  // Read source text into host storage before submitting it to the incremental
  // compiler.
  auto Buffer = llvm::MemoryBuffer::getFileOrSTDIN((SourceFile.getValue()));
  if (!Buffer) {
    llvm::errs() << Buffer.getError().message() << "\n";
    return 1;
  }
  SourceText = (*Buffer)->getBuffer().str();
  // Build the input's AST and IR without running its initializers.
  auto Parsed = Engine->Parse(SourceText);
  if (!Parsed) {
    llvm::logAllUnhandledErrors(Parsed.takeError(), llvm::errs(), "parse: ");
    return 1;
  }
  clang::FunctionDecl *Entry = nullptr;
  // extern "C" declarations can be nested in a LinkageSpecDecl.
  // extern "C" can wrap FunctionDecls in a LinkageSpecDecl; walk through those
  // contexts.
  std::vector<clang::DeclContext *> Contexts{Parsed->TUPart};
  for (size_t Index = 0; Index < Contexts.size(); ++Index) {
    for (clang::Decl *Declaration : Contexts[Index]->decls()) {
      if (auto *Linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(Declaration)) {
        Contexts.push_back(Linkage);
      }
      if (auto *Function = llvm::dyn_cast<clang::FunctionDecl>(Declaration)) {
        if (Function->getNameAsString() == "run" &&
            Function->doesThisDeclarationHaveABody()) {
          Entry = Function;
        }
      }
    }
  }
  // Reject an incompatible definition before executing its module or casting
  // its address.
  // Canonical builtin types accept aliases of int while preserving the required
  // ABI.
  if (!Entry || !Entry->isExternC() || Entry->isVariadic() ||
      Entry->isDeleted() ||
      Entry->getType()->castAs<clang::FunctionProtoType>()->getCallConv() !=
          clang::CC_C ||
      Entry->getNumParams() != 1 ||
      !Entry->getReturnType().getCanonicalType()->isSpecificBuiltinType(
          clang::BuiltinType::Int) ||
      !Entry->getParamDecl(0)
           ->getType()
           .getCanonicalType()
           ->isSpecificBuiltinType(clang::BuiltinType::Int)) {
    llvm::errs() << "entry must be defined as extern \"C\" int run(int)\n";
    return 1;
  }
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error E = Engine->Execute(*Parsed)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "execute: ");
    return 1;
  }
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  auto Address = Engine->getSymbolAddress(clang::GlobalDecl(Entry));
  if (!Address) {
    llvm::logAllUnhandledErrors(Address.takeError(), llvm::errs(), "lookup: ");
    return 1;
  }
  // The validated declaration permits int(int); keep Engine alive through every
  // native call.
  auto Run = Address->toPtr<int (*)(int)>();
  for (int N : Inputs) {
    llvm::outs() << "run(" << N << ") = " << Run(N) << "\n";
  }
  return 0;
}
