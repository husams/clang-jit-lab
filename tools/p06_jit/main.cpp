// p06_jit -- hand the interpreter your own LLJITBuilder (Section 6.3).
//
//   --abs     define host_secret() as an absolute symbol in the main JITDylib
//   --debugger  repeat what the default builder does before the platform is set
//             up: enableDebuggerSupport(J), which registers JIT'd code with
//             debuggers through __jit_debug_descriptor
//   --trace   attach a DefinitionGenerator that logs every symbol the main
//             JITDylib could not satisfy by itself, then delegates to the
//             process-symbol search
//
// host_secret() is `static`: the executable does not export it, so the JIT's
// normal process-symbol lookup cannot find it. --abs is the cure.
//
// Usage: build/bin/p06_jit [--abs] [--trace] [--debugger] inputs.repl [-- compiler flags]
#include "repllab.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include <algorithm>
#include <set>

using namespace clang;
using namespace llvm::orc;

// The GDB JIT interface: LLVM's debugger support appends one entry here for
// every JIT'd object it registers. Its head stays null if nobody registers.
struct jit_descriptor {
  uint32_t version, action_flag;
  void *relevant_entry, *first_entry;
};
extern "C" jit_descriptor __jit_debug_descriptor;

static int host_secret() { return 42; }

// An exported function, for contrast: no --abs needed.
extern "C" int host_exported() { return 7; }

namespace {

// Wraps another generator. tryToGenerate() is called with the set of symbols
// that no definition in the JITDylib satisfies; we log the names, then let the
// wrapped generator (the process-symbol search) have its turn.
class TraceGenerator : public DefinitionGenerator {
public:
  TraceGenerator(std::unique_ptr<DefinitionGenerator> Inner, std::set<std::string> &Seen)
      : Inner(std::move(Inner)), Seen(Seen) {}

  llvm::Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD, JITDylibLookupFlags Flags,
                            const SymbolLookupSet &Symbols) override {
    for (const auto &KV : Symbols)
      Seen.insert((*KV.first).str());
    return Inner->tryToGenerate(LS, K, JD, Flags, Symbols);
  }

private:
  std::unique_ptr<DefinitionGenerator> Inner;
  std::set<std::string> &Seen;
};

} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_jit: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  bool Abs = false, Trace = false, Debugger = false;
  std::vector<std::string> Files;
  for (const std::string &P : A.Positional) {
    if (P == "--abs")
      Abs = true;
    else if (P == "--debugger")
      Debugger = true;
    else if (P == "--trace")
      Trace = true;
    else
      Files.push_back(P);
  }

  LLJIT *JIT = nullptr;
  std::set<std::string> Seen;

  auto JB = std::make_unique<LLJITBuilder>();
  if (Debugger)
    JB->setPrePlatformSetup([](LLJIT &J) {
      consumeError(enableDebuggerSupport(J)); // what the default builder does
      return llvm::Error::success();
    });
  // Called once the LLJIT exists, before the interpreter runs any code: the
  // only moment a plain LLJITBuilder lets you reach the LLJIT&.
  JB->setNotifyCreatedCallback([&](LLJIT &J) -> llvm::Error {
    JIT = &J;
    JITDylib &Main = J.getMainJITDylib();
    if (Abs) {
      SymbolMap M;
      M[J.mangleAndIntern("host_secret")] = {ExecutorAddr::fromPtr(&host_secret),
                                              llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
      if (llvm::Error E = Main.define(absoluteSymbols(std::move(M))))
        return E;
    }
    if (Trace) {
      auto Process = DynamicLibrarySearchGenerator::GetForCurrentProcess(J.getDataLayout().getGlobalPrefix());
      if (!Process)
        return Process.takeError();
      Main.addGenerator(std::make_unique<TraceGenerator>(std::move(*Process), Seen));
    }
    return llvm::Error::success();
  });

  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->JITBuilder = std::move(JB);
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler, std::move(IEB)));
  llvm::outs() << "LLJIT reached through the callback: " << (JIT ? "yes" : "no") << "\n";

  for (const std::string &File : Files) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      llvm::outs() << "input: " << Code << "\n";
      repllab::run(*I, Code);
    }
  }

  llvm::outs() << "registered with debuggers (__jit_debug_descriptor): "
               << (__jit_debug_descriptor.first_entry ? "yes" : "no") << "\n";
  if (Trace) {
    llvm::outs() << "symbols the main JITDylib could not resolve itself:\n";
    for (const std::string &S : Seen)
      llvm::outs() << "  " << S << "\n";
  }
  return 0;
}
