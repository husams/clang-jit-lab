#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

struct Config {
  int verbosity;
  double scale;
  char name[16];
  int limits[4];
};

typedef void (*ReportFn)(const char *what, int value);

using namespace clang;

extern "C" void host_report(const char *What, int Value) {
  llvm::outs() << "  [host_report] " << What << " = " << Value << "\n";
}

// A pointer, spelled as a C++ expression the interpreter can parse.

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: config, callback, array, reverse"),
      llvm::cl::cat(Options), llvm::cl::init("config"));
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
  // Show the input (addresses masked), then run it.

  // The JIT declaration matches the host data layout above.
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
struct Config {
  int verbosity;
  double scale;
  char name[16];
  int limits[4];
};

typedef void (*ReportFn)(const char *what, int value);
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  if (Mode == "config") {
    Config Host = {1, 1.5, "host", {10, 20, 30, 40}};
    std::string AddressText;
    // Build a pointer expression using the live host address, not a copied
    // Config object.
    // The host and embedded declarations must agree on layout and object
    // lifetime.
    llvm::raw_string_ostream OutputForAddressText(AddressText);
    OutputForAddressText << llvm::format(
        "0x%llxUL",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>((&Host))));
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(auto *cfg = (Config *))cpp" + AddressText + R"cpp(;)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
cfg->verbosity
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
cfg->name
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
cfg->limits[2]
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(
cfg->scale *= 2;
cfg->verbosity = 7;
cfg->limits[3] = -1;
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
__builtin_strcpy(cfg->name, "jit wrote");
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host now sees: verbosity=" << Host.verbosity
                 << " scale=" << llvm::format("%g", Host.scale)
                 << " name=" << Host.name << " limits[3]=" << Host.limits[3]
                 << "\n";
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
sizeof(Config)
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host sizeof(Config) = " << sizeof(Config) << "\n";
  } else if (Mode == "callback") {
    ReportFn Fn = host_report;
    std::string AddressText2;
    llvm::raw_string_ostream OutputForAddressText2(AddressText2);
    OutputForAddressText2 << llvm::format(
        "0x%llxUL", static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(((const void *)Fn))));
    if (llvm::Error Error =
            I->ParseAndExecute(R"cpp(auto report = (ReportFn))cpp" +
                               AddressText2 + R"cpp(;)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
report("answer", 42);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(
for (int i = 1; i <= 3; ++i)
  report("square", i *i);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
  } else if (Mode == "array") {
    int Data[8] = {3, 1, 4, 1, 5, 9, 2, 6};
    std::string AddressText3;
    llvm::raw_string_ostream OutputForAddressText3(AddressText3);
    OutputForAddressText3 << llvm::format(
        "0x%llxUL",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>((Data))));
    if (llvm::Error Error =
            I->ParseAndExecute(R"cpp(int *data = (int *))cpp" + AddressText3 +
                               R"cpp(; const int n = 8;)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(
int sum = 0;
for (int i = 0; i < n; ++i)
  sum += data[i];
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
sum
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host computes the same sum: "
                 << std::accumulate(Data, Data + 8, 0) << "\n";
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(
for (int i = 0; i < n; ++i)
  data[i] *= data[i];
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "host array after the interpreter squared it:";
    for (int X : Data) {
      llvm::outs() << " " << X;
    }
    llvm::outs() << "\n";
  } else if (Mode == "reverse") {
    if (llvm::Error Error = I->ParseAndExecute(
            R"cpp(
Config jit_cfg = {3, 0.25, "made in jit", {1, 2, 3, 4}};
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
jit_cfg
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    } // an lvalue: V refers to the interpreter's variable
    auto *C = static_cast<Config *>(V.getPtr());
    llvm::outs() << "host reads jit_cfg: verbosity=" << C->verbosity
                 << " scale=" << llvm::format("%g", C->scale)
                 << " name=" << C->name << " limits[3]=" << C->limits[3]
                 << "\n";
    C->verbosity = 99;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
jit_cfg.verbosity
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    Value T;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
Config {
  5, 0.5, "temporary", {
  }
}
)cpp",
                                               &T)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    } // an rvalue: T owns a copy
    auto *Tmp = static_cast<Config *>(T.getPtr());
    llvm::outs() << "host reads the temporary: name=" << Tmp->name
                 << " owned=" << T.isManuallyAlloc()
                 << " same address as jit_cfg: " << (Tmp == C) << "\n";
  }
  return 0;
}
