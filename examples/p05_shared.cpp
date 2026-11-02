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

// Visible to the JIT: external linkage, defined in the executable.
extern "C" int host_counter = 10;
extern "C" const char *host_name = "the host";
extern "C" int host_twice(int X) { return 2 * X; }

// C++ linkage: Clang derives ABI-mangled names for these host definitions.
int host_cpp_add(int A, int B) { return A + B; }
namespace host {
int ns_scale(int X) { return 100 * X; }
} // namespace host

// Internal linkage: not an exported symbol, so the JIT cannot find it.
static int host_hidden(int X) { return X + 1; }

// Called FROM the interpreter; prints so the order of events is visible.
extern "C" void host_log(const char *Msg) {
  llvm::outs() << "  [host_log] " << Msg << "\n";
}

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: extern, cpp, static, jit2host"),
      llvm::cl::cat(Options), llvm::cl::init("extern"));
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

  // The JIT declarations below describe definitions already exported by this
  // process.
  // Both sides access the same objects; no state is copied by symbol
  // resolution.
  if (Mode == "extern") {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
extern "C" int host_counter;
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
extern "C" int host_twice(int);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
extern "C" const char *host_name;
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
extern "C" void host_log(const char *);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host sees host_counter = " << host_counter << "\n";
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_counter
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_counter += 5;
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host sees host_counter = " << host_counter
                 << " after the interpreter added 5\n";
    host_counter = 100;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_counter
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_twice(21)
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_name
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_log("called from interpreted code");
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    // Where does the interpreter's `host_counter` live? In the host.
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
&host_counter
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "&host_counter in the JIT == &host_counter in C++: "
                 << (V.convertTo<int *>() == &host_counter) << "\n";
  } else if (Mode == "cpp") {
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int host_cpp_add(int, int);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
namespace host {
int ns_scale(int);
}
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_cpp_add(2, 3)
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host::ns_scale(4)
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    // The interpreter's own function, found by IR name (the mangled name):
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int jit_sub(int a, int b) {
  return a - b;
}
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    // Resolve a runtime address; lookup may compile/link a module on demand.
    // The session owns code/storage; function-pointer types must match the ABI.
    auto AddressOrError = I->getSymbolAddress("_Z7jit_subii");
    if (!AddressOrError) {
      llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    auto Addr = std::move(*AddressOrError);
    auto *Sub = Addr.toPtr<int (*)(int, int)>();
    llvm::outs() << "host calls jit_sub(9, 4) = " << Sub(9, 4) << "\n";
  } else if (Mode == "static") {
    llvm::outs() << "host_hidden(1) in C++ = " << host_hidden(1) << "\n";
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int host_hidden(int);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
host_hidden(1)
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
  } else if (Mode == "jit2host") {
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int jit_total = 0;
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
void jit_add(int n) {
  jit_total += n;
}
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    auto AddressOrError2 = I->getSymbolAddress("_Z7jit_addi");
    if (!AddressOrError2) {
      llvm::logAllUnhandledErrors(AddressOrError2.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    auto Add = (*AddressOrError2).toPtr<void (*)(int)>();
    auto AddressOrError3 = I->getSymbolAddress("jit_total");
    if (!AddressOrError3) {
      llvm::logAllUnhandledErrors(AddressOrError3.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    auto Total = (*AddressOrError3).toPtr<int *>();
    for (int N : {1, 2, 3}) {
      Add(N);
    }
    llvm::outs() << "host called jit_add 3 times; jit_total read from C++ = "
                 << *Total << "\n";
    *Total = 1000;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
jit_total
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
  }
  return 0;
}
