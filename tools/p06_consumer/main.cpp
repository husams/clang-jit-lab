// p06_consumer -- subclass Interpreter with the protected constructor and a
// custom ASTConsumer that logs every declaration the parser hands over
// (Section 6.1). The same inputs then run through a stock Interpreter::create()
// to show what the subclass gave up.
//
// Usage: build/bin/p06_consumer inputs.repl [-- compiler flags]
#include "repllab.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

using namespace clang;

namespace {

// Logs what the parser produces. HandleTopLevelDecl runs once per top-level
// declaration group while an input is being parsed; HandleTranslationUnit once
// per input (the incremental parser ends every input like a tiny translation
// unit).
class LogConsumer : public ASTConsumer {
  unsigned &Input;

public:
  explicit LogConsumer(unsigned &Input) : Input(Input) {}

  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    for (Decl *D : DG) {
      llvm::outs() << "  [consumer] input " << Input << ": " << D->getDeclKindName();
      if (auto *ND = llvm::dyn_cast<NamedDecl>(D))
        llvm::outs() << " '" << ND->getQualifiedNameAsString() << "'";
      llvm::outs() << "\n";
    }
    return true;
  }

  void HandleTranslationUnit(ASTContext &) override {
    llvm::outs() << "  [consumer] input " << Input << ": end of translation unit\n";
  }
};

// The protected constructor is only reachable from a subclass. It takes the
// executor builder (nullptr = default) and the consumer; it does NOT parse the
// runtime prelude that Interpreter::create() adds afterwards.
class LoggingInterpreter : public Interpreter {
public:
  LoggingInterpreter(std::unique_ptr<CompilerInstance> CI, llvm::Error &Err, std::unique_ptr<ASTConsumer> C)
      : Interpreter(std::move(CI), Err, /*IEB=*/nullptr, std::move(C)) {}
};

} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_consumer: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  std::vector<std::string> Inputs;
  for (const std::string &File : A.Positional)
    for (std::string &Code : repllab::replInputs(repllab::readFile(File)))
      if (!llvm::StringRef(Code).starts_with("//"))
        Inputs.push_back(std::move(Code));

  // 1. The subclass. The consumer needs to know the input number, so the
  //    counter lives outside both.
  unsigned Input = 0;
  llvm::outs() << "== LoggingInterpreter (protected constructor + LogConsumer)\n";
  {
    auto CI = ExitOnErr(repllab::makeCompiler(A.Compiler));
    llvm::Error Err = llvm::Error::success();
    LoggingInterpreter I(std::move(CI), Err, std::make_unique<LogConsumer>(Input));
    ExitOnErr(std::move(Err));

    for (const std::string &Code : Inputs) {
      ++Input;
      llvm::outs() << "input " << Input << ": " << Code << "\n";
      Value V;
      repllab::run(I, Code, &V);
      if (V.isValid())
        llvm::outs() << "  value: " << repllab::valueString(V);
    }

    unsigned Undone = 0;
    while (true) { // Undo returns an Error: success means "undid one more input"
      if (llvm::Error E = I.Undo()) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "undo: " << Undone << " inputs undone, then \"No input left to undo\"\n";
  }

  // 2. The stock interpreter, same inputs, no consumer.
  llvm::outs() << "\n== Interpreter::create() (runtime prelude + value printing)\n";
  {
    auto I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
    unsigned N = 0;
    for (const std::string &Code : Inputs) {
      ++N;
      llvm::outs() << "input " << N << ": " << Code << "\n";
      repllab::run(*I, Code);
    }
    unsigned Undone = 0;
    while (true) {
      if (llvm::Error E = I->Undo()) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "undo: " << Undone << " inputs undone, then \"No input left to undo\"\n";
  }
  return 0;
}
