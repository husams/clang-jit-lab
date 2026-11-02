// p06_eval -- JIT code that calls back into the live interpreter (Section 6.7).
//
// The host exports four C functions (macOS exports an executable's global
// symbols, so JIT code resolves them by name):
//
//   repl_eval(code)        runs `code` in the interpreter right now, from inside
//                          the input that is executing (re-entrant)
//   repl_defer(code)       only queues `code`; the host runs the queue after the
//                          current input has returned
//   repl_eval_double(code) re-entrant, returns the value of an expression
//   repl_lookup(name)      address of a symbol the interpreter defined (IR name),
//                          so JIT code can call what it has just generated
//
// Usage: build/bin/p06_eval inputs.repl [-- compiler flags]
#include "repllab.h"

using namespace clang;

static Interpreter *Live = nullptr;
static std::vector<std::string> Deferred;
static int Depth = 0;

extern "C" int repl_eval(const char *Code) {
  ++Depth;
  llvm::outs() << "  [host] repl_eval (depth " << Depth << "): " << Code << "\n";
  bool OK = repllab::run(*Live, Code);
  llvm::outs() << "  [host] repl_eval returns " << (OK ? 0 : 1) << "\n";
  --Depth;
  return OK ? 0 : 1;
}

extern "C" int repl_defer(const char *Code) {
  llvm::outs() << "  [host] repl_defer queued: " << Code << "\n";
  Deferred.push_back(Code);
  return 0;
}

extern "C" double repl_eval_double(const char *Code) {
  Value V;
  if (llvm::Error E = Live->ParseAndExecute(Code, &V)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ");
    return -1;
  }
  return V.isValid() && V.getKind() == Value::K_Double ? V.getDouble() : -2;
}

extern "C" void *repl_lookup(const char *Name) {
  auto Addr = Live->getSymbolAddress(Name);
  if (!Addr) {
    llvm::consumeError(Addr.takeError());
    return nullptr;
  }
  return Addr->toPtr<void *>();
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_eval: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  Live = I.get();

  for (const std::string &File : A.Positional) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      llvm::outs() << "input: " << (Code.size() > 72 ? Code.substr(0, 72) + " ..." : Code) << "\n";
      repllab::run(*I, Code);
      // The safe pattern: the input has returned, nothing is executing in the
      // JIT, so it is fine to feed the interpreter again.
      for (size_t N = 0; N < Deferred.size(); ++N) {
        std::string Q = Deferred[N];
        llvm::outs() << "  [host] running deferred: " << Q << "\n";
        repllab::run(*I, Q);
      }
      Deferred.clear();
    }
  }
  return 0;
}
