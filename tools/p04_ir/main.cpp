// p04_ir -- print the LLVM IR a PartialTranslationUnit carries (Section 4.7).
//
// Usage: build/bin/p04_ir inputs.repl [--full] [--ll=out/file.ll] [-- compiler flags]
//   Parses every input, prints (trimmed, or --full; or writes unfiltered to a file) the module's IR BEFORE
//   Execute -- Execute moves the module into the JIT -- then executes it.
#include "clang/Interpreter/Interpreter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Regex.h"

#include "repllab.h"

using namespace clang;

// Raw addresses show up as big decimal constants (inttoptr (i64 38403440896 ...)).
static std::string maskNumbers(llvm::StringRef IR) {
  static const llvm::Regex Big("i64 [0-9]{8,}");
  std::string Out = IR.str(), Err;
  while (Big.match(Out))
    Out = Big.sub("i64 <addr>", Out, &Err);
  return Out;
}

// Drop the boilerplate every module carries (target, flags, attribute groups).
static std::string trim(llvm::StringRef IR) {
  std::string Out;
  bool LastBlank = true;
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  std::string Masked = maskNumbers(IR);
  llvm::StringRef(Masked).split(Lines, '\n');
  for (llvm::StringRef L : Lines) {
    if (L.starts_with("!") || L.starts_with("attributes ") || L.starts_with("source_filename") ||
        L.starts_with("target ") || L.starts_with("; ModuleID"))
      continue;
    bool Blank = L.empty();
    if (Blank && LastBlank)
      continue;
    LastBlank = Blank;
    Out += L.str() + "\n";
  }
  return Out;
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_ir: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.empty()) {
    llvm::errs() << "usage: p04_ir inputs.repl [--full] [--ll=out/file.ll] [-- compiler flags]\n";
    return 2;
  }
  std::string LL;
  bool Full = false;
  for (size_t K = 1; K < A.Positional.size(); ++K)
    if (llvm::StringRef(A.Positional[K]).starts_with("--ll="))
      LL = A.Positional[K].substr(5);
    else if (A.Positional[K] == "--full")
      Full = true;
  std::unique_ptr<llvm::raw_fd_ostream> File;
  if (!LL.empty()) {
    std::error_code EC;
    File = std::make_unique<llvm::raw_fd_ostream>(LL, EC);
    if (EC) {
      llvm::errs() << "p04_ir: cannot open " << LL << ": " << EC.message() << "\n";
      return 1;
    }
  }

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  unsigned N = 0;
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[0]))) {
    ++N;
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::logAllUnhandledErrors(PTU.takeError(), llvm::errs(), "error: ");
      continue;
    }
    std::string IR;
    llvm::raw_string_ostream IROS(IR);
    if (PTU->TheModule) // a syntax-only run (-fsyntax-only) generates no code
      PTU->TheModule->print(IROS, nullptr);
    if (File) {
      *File << "; ---- input " << N << ": " << Code << "\n" << IR;
    } else {
      llvm::outs() << "; ---- input " << N << ": " << Code << "\n";
      llvm::outs() << (Full ? maskNumbers(IR) : trim(IR));
    }
    if (PTU->TheModule)
      ExitOnErr(I->Execute(*PTU));
  }
  return 0;
}
