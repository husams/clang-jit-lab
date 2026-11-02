#include <algorithm>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
  TraceGenerator(std::unique_ptr<DefinitionGenerator> Inner,
                 std::set<std::string> &Seen)
      : Inner(std::move(Inner)), Seen(Seen) {}

  llvm::Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                            JITDylibLookupFlags Flags,
                            const SymbolLookupSet &Symbols) override {
    for (const auto &KV : Symbols) {
      Seen.insert((*KV.first).str());
    }
    return Inner->tryToGenerate(LS, K, JD, Flags, Symbols);
  }

private:
  std::unique_ptr<DefinitionGenerator> Inner;
  std::set<std::string> &Seen;
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <stdio.h>
)cpp",
    R"cpp(
extern "C" int host_exported();
)cpp",
    R"cpp(
extern "C" int host_secret();
)cpp",
    R"cpp(
printf("exported: %d\n", host_exported());
)cpp",
    R"cpp(
printf("secret: %d\n", host_secret());
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<bool> Abs(
      "abs", llvm::cl::desc("Publish host_secret as an absolute symbol"),
      llvm::cl::cat(Options), llvm::cl::init(false));
  llvm::cl::opt<bool> Trace("trace",
                            llvm::cl::desc("Trace unresolved symbol requests"),
                            llvm::cl::cat(Options), llvm::cl::init(false));
  llvm::cl::opt<bool> Debugger("debugger",
                               llvm::cl::desc("Enable debugger registration"),
                               llvm::cl::cat(Options), llvm::cl::init(false));
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

  LLJIT *JIT = nullptr;
  std::set<std::string> Seen;

  auto JB = std::make_unique<LLJITBuilder>();
  if (Debugger) {
    // A custom builder replaces defaults; register debugger support before
    // platform setup.
    JB->setPrePlatformSetup([](LLJIT &J) {
      consumeError(enableDebuggerSupport(J)); // what the default builder does
      return llvm::Error::success();
    });
  }
  // Called once the LLJIT exists, before the interpreter runs any code: the
  // only moment a plain LLJITBuilder lets you reach the LLJIT&.
  JB->setNotifyCreatedCallback([&](LLJIT &J) -> llvm::Error {
    JIT = &J;
    JITDylib &Main = J.getMainJITDylib();
    if (Abs) {
      SymbolMap M;
      // Intern the target's symbol spelling and associate it with this
      // process-local callable address.
      M[J.mangleAndIntern("host_secret")] = {
          ExecutorAddr::fromPtr(&host_secret),
          llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
      // Define the private callback directly in the main JITDylib so fallback
      // lookup is unnecessary.
      if (llvm::Error E = Main.define(absoluteSymbols(std::move(M)))) {
        return E;
      }
    }
    if (Trace) {
      // Fallback lookup searches exported symbols in the current process with
      // the target prefix.
      auto Process = DynamicLibrarySearchGenerator::GetForCurrentProcess(
          J.getDataLayout().getGlobalPrefix());
      if (!Process) {
        return Process.takeError();
      }
      Main.addGenerator(
          std::make_unique<TraceGenerator>(std::move(*Process), Seen));
    }
    return llvm::Error::success();
  });

  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->JITBuilder = std::move(JB);
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
      clang::Interpreter::create(std::move(*CompilerOrError), std::move(IEB));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);
  llvm::outs() << "LLJIT reached through the callback: " << (JIT ? "yes" : "no")
               << "\n";

  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      llvm::outs() << "input: " << Code << "\n";
      // Compile this input, then submit its module and run its initializers.
      if (llvm::Error Error = I->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
    }
  }

  llvm::outs() << "registered with debuggers (__jit_debug_descriptor): "
               << (__jit_debug_descriptor.first_entry ? "yes" : "no") << "\n";
  if (Trace) {
    llvm::outs() << "symbols the main JITDylib could not resolve itself:\n";
    for (const std::string &S : Seen) {
      llvm::outs() << "  " << S << "\n";
    }
  }
  return 0;
}
