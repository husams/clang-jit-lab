// p04_complete -- the code-completion API, driven exactly like clang-repl's
// ReplListCompleter does (Section 4.10).
//
// Usage: build/bin/p04_complete [--limit=N] setup.repl 'buffer' ['buffer' ...] [-- compiler flags]
//   setup.repl runs in the main interpreter; every buffer is then completed at
//   its end (Tab at the end of the line), using a fresh interpreter per request.
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/CodeCompletion.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

#include <algorithm>

using namespace clang;

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_complete: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  size_t Limit = 8;
  std::vector<std::string> Pos;
  for (const std::string &S : A.Positional) {
    if (llvm::StringRef(S).starts_with("--limit="))
      Limit = std::stoul(S.substr(8));
    else
      Pos.push_back(S);
  }
  if (Pos.size() < 2) {
    llvm::errs() << "usage: p04_complete [--limit=N] setup.repl 'buffer' ... [-- compiler flags]\n";
    return 2;
  }

  // The main interpreter: holds the session's declarations.
  std::unique_ptr<Interpreter> Main = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string &Code : repllab::replInputs(repllab::readFile(Pos[0])))
    ExitOnErr(Main->ParseAndExecute(Code));

  for (size_t K = 1; K < Pos.size(); ++K) {
    llvm::StringRef Buffer = Pos[K];
    // A fresh CompilerInstance + Interpreter for every request.
    std::unique_ptr<CompilerInstance> CI = ExitOnErr(repllab::makeCompiler(A.Compiler));
    std::unique_ptr<Interpreter> Fresh = ExitOnErr(Interpreter::create(std::move(CI)));

    unsigned Line = std::count(Buffer.begin(), Buffer.end(), '\n') + 1;
    unsigned Col = Buffer.size() + 1; // as ClangRepl.cpp: Pos + 1 (single-line buffers)

    ReplCodeCompleter CC;
    std::vector<std::string> Results;
    CC.codeComplete(Fresh->getCompilerInstance(), Buffer, Line, Col, Main->getCompilerInstance(), Results);

    // ClangRepl.cpp keeps the candidates that start with Prefix and inserts
    // only the part after it.
    std::vector<std::string> Matches;
    for (const std::string &R : Results)
      if (R.find(CC.Prefix) == 0)
        Matches.push_back(R);
    std::sort(Matches.begin(), Matches.end());
    Matches.erase(std::unique(Matches.begin(), Matches.end()), Matches.end());

    llvm::outs() << "buffer: '" << Buffer << "'\n";
    llvm::outs() << "  Prefix: '" << CC.Prefix << "'  raw results: " << Results.size()
                 << "  matching: " << Matches.size() << "\n";
    for (size_t R = 0; R < Matches.size() && R < Limit; ++R)
      llvm::outs() << "  " << Matches[R] << "   (inserts '" << Matches[R].substr(CC.Prefix.size()) << "')\n";
  }
  return 0;
}
