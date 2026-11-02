#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int x = 42;
)cpp",
    R"cpp(
double d = 2.5;
)cpp",
    R"cpp(
struct P {
  int a;
  int b;
};
)cpp",
    R"cpp(
P p{1, 2};
)cpp",
    R"cpp(
void nothing() {
}
)cpp",
    R"cpp(
x
)cpp",
    R"cpp(
x + 1
)cpp",
    R"cpp(
d * 2
)cpp",
    R"cpp(
(x > 3)
)cpp",
    R"cpp(
'A'
)cpp",
    R"cpp(
"hello"
)cpp",
    R"cpp(
nothing()
)cpp",
    R"cpp(
nothing
)cpp",
    R"cpp(
p
)cpp",
    R"cpp(
P{3, 4}
)cpp",
    R"cpp(
&x
)cpp",
    R"cpp(
static_cast<void *>(&x)
)cpp",
    R"cpp(
nullptr
)cpp",
    R"cpp(
int arr[3] = {7, 8, 9};
)cpp",
    R"cpp(
arr
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<bool> Reuse(
      "reuse", llvm::cl::desc("Reuse one Value across several inputs"),
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

  if (Reuse) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int x = 42;
void nothing() {
}
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
    // One Value reused for several inputs: only an input that produces a value
    // writes to it. A declaration leaves the previous contents alone.
    llvm::outs() << "\nreusing one Value:\n";
    Value V;
    for (const char *Code : {R"cpp(
x
)cpp",
                             R"cpp(
int y = 1;
)cpp",
                             R"cpp(
nothing()
)cpp",
                             R"cpp(
y + 1
)cpp",
                             R"cpp(
int z = 2;
)cpp"}) {
      llvm::Error InputError2 = I->ParseAndExecute(Code, &V);
      bool InputAccepted1 = !InputError2;
      if (InputError2) {
        llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                    "error: ");
      }
      if (!InputAccepted1) {
        continue;
      }
      std::string ValueText;
      if (!V.isValid()) {
        ValueText = "<invalid Value>\n";
      } else {
        llvm::raw_string_ostream OutputForValueText(ValueText);
        V.print(OutputForValueText);
      }
      const char *KindName = "?";
      // The kind identifies active storage; getType() retains the expression's
      // Clang type.
      switch (V.getKind()) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    KindName = "K_" #name;                                                     \
    break;
        REPL_BUILTIN_TYPES
#undef X
      case clang::Value::K_Void:
        KindName = "K_Void";
        break;
      case clang::Value::K_PtrOrObj:
        KindName = "K_PtrOrObj";
        break;
      case clang::Value::K_Unspecified:
        KindName = "K_Unspecified";
        break;
      }
      llvm::outs()
          << "  after "
          << (llvm::formatv(("{0,-" + std::to_string((12)) + "}").c_str(),
                            (Code))
                  .str())
          << "-> "
          << (llvm::formatv(("{0,-" + std::to_string((15)) + "}").c_str(),
                            (KindName))
                  .str())
          << (V.hasValue() ? ValueText : std::string("(no data)\n"));
    }
    return 0;
  }

  llvm::outs() << (llvm::formatv(("{0,-" + std::to_string((38)) + "}").c_str(),
                                 ("input"))
                       .str())
               << (llvm::formatv(("{0,-" + std::to_string((8)) + "}").c_str(),
                                 ("isValid"))
                       .str())
               << (llvm::formatv(("{0,-" + std::to_string((15)) + "}").c_str(),
                                 ("kind"))
                       .str())
               << (llvm::formatv(("{0,-" + std::to_string((9)) + "}").c_str(),
                                 ("hasValue"))
                       .str())
               << (llvm::formatv(("{0,-" + std::to_string((7)) + "}").c_str(),
                                 ("manual"))
                       .str())
               << "type\n";
  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      // A fresh Value starts invalid, avoiding a stale result after a
      // declaration-only input.
      Value V; // a fresh Value for every input: default constructed = invalid
      llvm::Error InputError4 = I->ParseAndExecute(Code, &V);
      bool InputAccepted3 = !InputError4;
      if (InputError4) {
        llvm::logAllUnhandledErrors(std::move(InputError4), llvm::errs(),
                                    "error: ");
      }
      if (!InputAccepted3) {
        continue;
      }
      std::string Type =
          V.isValid()
              ? V.getType().getAsString(V.getASTContext().getPrintingPolicy())
              : "-";
      const char *KindName2 = "?";
      switch (V.getKind()) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    KindName2 = "K_" #name;                                                    \
    break;
        REPL_BUILTIN_TYPES
#undef X
      case clang::Value::K_Void:
        KindName2 = "K_Void";
        break;
      case clang::Value::K_PtrOrObj:
        KindName2 = "K_PtrOrObj";
        break;
      case clang::Value::K_Unspecified:
        KindName2 = "K_Unspecified";
        break;
      }
      llvm::outs()
          << (llvm::formatv(("{0,-" + std::to_string((38)) + "}").c_str(),
                            (Code))
                  .str())
          << (llvm::formatv(("{0,-" + std::to_string((8)) + "}").c_str(),
                            (V.isValid() ? "yes" : "no"))
                  .str())
          << (llvm::formatv(("{0,-" + std::to_string((15)) + "}").c_str(),
                            (KindName2))
                  .str())
          << (llvm::formatv(("{0,-" + std::to_string((9)) + "}").c_str(),
                            (V.hasValue() ? "yes" : "no"))
                  .str())
          << (llvm::formatv(("{0,-" + std::to_string((7)) + "}").c_str(),
                            (V.isManuallyAlloc() ? "yes" : "no"))
                  .str())
          << Type << "\n";
    }
  }

  return 0;
}
