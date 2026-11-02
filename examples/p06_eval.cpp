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

static Interpreter *Live = nullptr;
static std::vector<std::string> Deferred;
static int Depth = 0;

extern "C" int evaluateSource(const char *Code) {
  ++Depth;
  llvm::outs() << "  [host] evaluateSource (depth " << Depth << "): " << Code
               << "\n";
  // Re-enter only from the demonstrated execution phase, after the outer parse
  // has finished.
  // The same interpreter cannot be parsed concurrently or used during its
  // teardown.
  llvm::Error InputError2 = Live->ParseAndExecute(Code);
  bool InputAccepted1 = !InputError2;
  if (InputError2) {
    llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                "error: ");
  }
  bool OK = InputAccepted1;
  llvm::outs() << "  [host] evaluateSource returns " << (OK ? 0 : 1) << "\n";
  --Depth;
  return OK ? 0 : 1;
}

extern "C" int deferSource(const char *Code) {
  llvm::outs() << "  [host] deferSource queued: " << Code << "\n";
  // Copy the request without compiling; the host drains it after the active
  // input returns.
  Deferred.push_back(Code);
  return 0;
}

extern "C" double evaluateDouble(const char *Code) {
  Value V;
  // Capture into this existing Value, avoiding return-by-value moves in LLVM
  // 22.1.8.
  if (llvm::Error E = Live->ParseAndExecute(Code, &V)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ");
    return -1;
  }
  return V.isValid() && V.getKind() == Value::K_Double ? V.getDouble() : -2;
}

extern "C" void *lookupFunction(const char *Name) {
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  auto Addr = Live->getSymbolAddress(Name);
  if (!Addr) {
    llvm::consumeError(Addr.takeError());
    return nullptr;
  }
  return Addr->toPtr<void *>();
}

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <stdio.h>
)cpp",
    R"cpp(
extern "C" int evaluateSource(const char *code);
)cpp",
    R"cpp(
extern "C" void *lookupFunction(const char *name);
)cpp",
    R"cpp(
evaluateSource(R"definition(
int made_by_eval(int x) {
  return x + 1;
}
)definition");
)cpp",
    R"cpp(
printf("made_by_eval(41) = %d\n", made_by_eval(41));
)cpp",
    R"cpp(
#include <string>
)cpp",
    R"cpp(
long gen_and_call(int n, long x) {
  std::string name = "pow" + std::to_string(n), body = "x";
  for (int k = 1; k < n; ++k)
    body += " * x";
  std::string source = R"function(
extern "C" long )function" + name + R"function((long x) {
  return )function" + body + R"function(;
}
)function";
  evaluateSource(source.c_str());
  return ((long (*)(long))lookupFunction(name.c_str()))(x);
}
)cpp",
    R"cpp(
for (int n = 1; n <= 4; ++n)
  printf("pow%d(3) = %ld\n", n, gen_and_call(n, 3));
)cpp",
};

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
  Live = I.get();

  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      llvm::outs() << "input: "
                   << (Code.size() > 72 ? Code.substr(0, 72) + " ..." : Code)
                   << "\n";
      if (llvm::Error Error = I->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      // The safe pattern: the input has returned, nothing is executing in the
      // JIT, so it is fine to feed the interpreter again.
      // Drain queued requests while the session is idle; a request may enqueue
      // more source.
      for (size_t N = 0; N < Deferred.size(); ++N) {
        std::string Q = Deferred[N];
        llvm::outs() << "  [host] running deferred: " << Q << "\n";
        if (llvm::Error Error = I->ParseAndExecute(Q)) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
      }
      Deferred.clear();
    }
  }
  return 0;
}
