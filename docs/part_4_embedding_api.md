# Part 4 — Clang Compiler Services

[← Part 3 — Connecting Host and JIT](part_3_under_the_hood.md) | [Part 5 — Values, Objects and Lifetimes →](part_5_values_and_data.md)

Use the compiler services already inside your engine to explain errors, complete partial input, find declarations and understand types before calling generated code.

## Section 4.1 — Errors and diagnostics

A terminal diagnostic is enough for a small experiment, but an editor or service needs more: which input failed, what Clang reported and whether the application should continue. The host needs to handle the API failure while also collecting the compiler's explanation.

Compare LLVM's error-return mechanisms with Clang's diagnostic callback. Consume each returned error, then capture messages through a `DiagnosticConsumer` so the host can inspect them as data. Follow a failed input and a later valid one to separate the diagnostic, the failed operation and the session state left afterwards.

An `llvm::Error` reports that an operation failed; Clang's diagnostics explain the source problem. Handle both channels when building an interface around the interpreter.

| Style | Code | Use when |
|-------|------|----------|
| to a string | `llvm::toString(std::move(E))` | you only want the message |
| a handler | `llvm::handleAllErrors(std::move(E), [](const llvm::ErrorInfoBase &EIB) { ... EIB.message() ... })` | you want to treat error kinds differently |
| log it | `llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ")` | you just want it on a stream |

For an `llvm::Expected<T>`, test the result before using it and take its error on failure. The reference tool demonstrates these handling paths:

```bash
build/bin/p04_errors
```

```text expected
-- toString
In file included from <<< inputs >>>:1:
input_line_1:1:9: error: expected expression
    1 | int a = ;
      |         ^
message: Parsing failed.
-- handleAllErrors
input_line_2:1:9: error: use of undeclared identifier 'nope'
    1 | int b = nope;
      |         ^~~~
message: Parsing failed.
-- logAllUnhandledErrors
input_line_3:1:9: warning: missing terminating ' character [-Winvalid-pp-token]
    1 | int c = 'x
      |         ^
input_line_3:1:9: error: expected expression
input_line_3:1:11: error: expected ';' after top level declarator
    1 | int c = 'x
      |           ^
      |           ;
error: Parsing failed.
-- Expected from Parse
input_line_4:1:9: error: use of undeclared identifier 'nope'
    1 | int d = nope;
      |         ^~~~
Parse succeeded: no
message: Parsing failed.
-- Undo
Undo(1) on a fresh interpreter: Operation failed. No input left to undo
Undo(5) with two inputs: Operation failed. Wanted to undo 5 inputs, only have 2.
Undo(2) with two inputs:
(llvm::toString of a success is the empty string)
-- the interpreter still works
(int) 7
```

The detailed source excerpt comes from Clang's diagnostic renderer. The returned error may only say `Parsing failed.` API errors such as an invalid undo count carry their own messages instead. A successful operation can also issue warnings, so the error result alone cannot describe everything Clang reported.

Install a `DiagnosticConsumer` to receive those diagnostics as data:

<!-- runnable-example: 4.1 -->

`examples/p04_diag.cpp`:

```cpp
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
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

namespace {
// Collects diagnostics into plain strings. The interpreter owns the
// CompilerInstance (and so the DiagnosticsEngine), but not this consumer.
class CollectingConsumer : public DiagnosticConsumer {
public:
  std::vector<std::string> Seen;

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level,
                                         Info); // keeps NumErrors / NumWarnings
    const char *Name = "other";
    switch (Level) {
    case DiagnosticsEngine::Note:
      Name = "note";
      break;
    case DiagnosticsEngine::Remark:
      Name = "remark";
      break;
    case DiagnosticsEngine::Warning:
      Name = "warning";
      break;
    case DiagnosticsEngine::Error:
      Name = "error";
      break;
    case DiagnosticsEngine::Fatal:
      Name = "fatal";
      break;
    default:
      break;
    }
    llvm::SmallString<256> Msg;
    // FormatDiagnostic expands Clang's message arguments without printing to
    // stderr.
    Info.FormatDiagnostic(Msg);
    std::string Where = "<no location>";
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      // Resolve the diagnostic location only when a SourceManager and valid
      // location exist.
      PresumedLoc P =
          Info.getSourceManager().getPresumedLoc(Info.getLocation());
      if (P.isValid()) {
        Where = (llvm::Twine(P.getFilename()) + ":" + llvm::Twine(P.getLine()) +
                 ":" + llvm::Twine(P.getColumn()))
                    .str();
      }
    }
    Seen.push_back(std::string(Name) + " | " + Where + " | " + Msg.str().str());
  }
};
} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int good = 1;
)cpp",
    R"cpp(
int x = unknown_name;
)cpp",
    R"cpp(
int y = "text";
)cpp",
    R"cpp(
int w = (int)3.5 / 0;
)cpp",
    R"cpp(
void f() {
  int unused;
}
)cpp",
    R"cpp(
good + 1
)cpp",
    R"cpp(
struct P {
  int a;
};
P p;
p.b
)cpp",
    R"cpp(
int good = 2;
)cpp",
    R"cpp(
good + 41
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

  CollectingConsumer Consumer;
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
                                "error: ");
    return 1;
  }
  std::unique_ptr<CompilerInstance> CI = std::move(*CompilerOrError);
  // Before Interpreter::create(): the prelude is parsed inside create().
  // Borrow this stack consumer rather than deleting it; install before prelude
  // diagnostics.
  CI->getDiagnostics().setClient(&Consumer, /*ShouldOwnClient=*/false);
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = Interpreter::create(std::move(CI));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> Interp = std::move(*InterpreterOrError);
  llvm::outs() << "after create(): " << Consumer.Seen.size()
               << " diagnostics\n";

  unsigned N = 0, Failed = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    Consumer.Seen.clear();
    llvm::outs() << "[" << N << "] " << Code << "\n";
    std::string Status = "ok";
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      Status = "failed: " + llvm::toString(std::move(E)); // consumes the Error
      ++Failed;
    }
    llvm::outs() << "    -> " << Status << "\n";
    for (const std::string &D : Consumer.Seen) {
      llvm::outs() << "    " << D << "\n";
    }
  }
  llvm::outs() << "inputs=" << N << " failed=" << Failed << "\n";
  return 0;
}
```

`examples/p04_errors.cpp`:

```cpp
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

  // An llvm::Error must be consumed exactly once; destroying an unchecked one
  // aborts the process in assertion builds. Each style below consumes it.
  llvm::outs() << "-- toString\n";
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int a = ;
)cpp")) {
    // toString consumes the Error and returns its message; Clang diagnostics
    // are separate.
    llvm::outs() << "message: " << llvm::toString(std::move(E)) << "\n";
  }

  llvm::outs() << "-- handleAllErrors\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int b = nope;
)cpp")) {
    // Dispatch every contained error to a handler, consuming the checked Error.
    llvm::handleAllErrors(std::move(E), [](const llvm::ErrorInfoBase &EIB) {
      llvm::outs() << "message: " << EIB.message() << "\n";
    });
  }

  llvm::outs() << "-- logAllUnhandledErrors\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
int c = 'x
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "error: ");
  }

  llvm::outs() << "-- Expected from Parse\n";
  // Build the input's AST and IR without running its initializers.
  llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(R"cpp(
int d = nope;
)cpp");
  llvm::outs() << "Parse succeeded: " << (PTU ? "yes" : "no") << "\n";
  if (!PTU) {
    llvm::outs() << "message: " << llvm::toString(PTU.takeError()) << "\n";
  }

  llvm::outs() << "-- Undo\n";
  llvm::outs() << "Undo(1) on a fresh interpreter: ";
  // Remove recent accepted inputs and their symbols; earlier writes and I/O
  // remain.
  llvm::outs() << llvm::toString(I->Undo()) << "\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int e = 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f = 2;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  llvm::outs() << "Undo(5) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(5)) << "\n";
  llvm::outs() << "Undo(2) with two inputs: ";
  llvm::outs() << llvm::toString(I->Undo(2)) << "\n";
  llvm::outs() << "(llvm::toString of a success is the empty string)\n";

  llvm::outs() << "-- the interpreter still works\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int g = 7;
g
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_diag.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_diag
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_errors.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_errors
out/p04_diag -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr -Wall
out/p04_errors -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_diag.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_diag
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_errors.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_errors
out/p04_diag -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)" -Wall
out/p04_errors -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

`HandleDiagnostic` receives the severity and diagnostic information. The consumer formats the message and obtains a source location when a `SourceManager` is available. `ShouldOwnClient=false` keeps ownership with the host; the consumer must therefore outlive its use by the compiler. Installing it before `Interpreter::create` also covers the runtime prelude.

Read the fixture and predict which inputs can warn while succeeding:

```bash
cat manifests/p04_diag.repl
```

```text expected
// Part 4.4 -- inputs that produce warnings, errors and a clean recovery.
int good = 1;
int x = unknown_name;
int y = "text";
int w = (int)3.5 / 0;
void f() { int unused; }
good + 1
struct P { int a; }; P p; p.b
int good = 2;
good + 41
```

Enable `-Wall` and compare the API status with the collected messages:

```bash
build/bin/p04_diag manifests/p04_diag.repl -- -Wall
```

```text expected
after create(): 0 diagnostics
[1] // Part 4.4 -- inputs that produce warnings, errors and a clean recovery.
    -> ok
[2] int good = 1;
    -> ok
[3] int x = unknown_name;
    -> failed: Parsing failed.
    error | input_line_3:1:9 | use of undeclared identifier 'unknown_name'
[4] int y = "text";
    -> failed: Parsing failed.
    error | input_line_4:1:5 | cannot initialize a variable of type 'int' with an lvalue of type 'const char[5]'
[5] int w = (int)3.5 / 0;
    -> ok
    warning | input_line_5:1:18 | division by zero is undefined
[6] void f() { int unused; }
    -> ok
    warning | input_line_6:1:16 | unused variable 'unused'
[7] good + 1
(int) 2
    -> ok
[8] struct P { int a; }; P p; p.b
    -> failed: Parsing failed.
    error | input_line_8:1:29 | no member named 'b' in 'P'
[9] int good = 2;
    -> failed: Parsing failed.
    error | input_line_9:1:5 | redefinition of 'good'
    note | input_line_2:1:5 | previous definition is here
[10] good + 41
(int) 42
    -> ok
inputs=10 failed=4
```

The consumer replaces the default renderer, giving the host control over presentation. A warning can accompany `ok`, while a redefinition produces an error and a separate note pointing to the earlier declaration. The buffer names `input_line_N` identify submitted inputs, including rejected ones.

Reduce that distinction to three inputs:

```bash
mkdir -p out
printf '%s\n' 'int ok = 1;' 'int d = 1 / 0;' 'int bad = nope;' > out/p04_verify.repl
build/bin/p04_diag out/p04_verify.repl
```

```text expected
after create(): 0 diagnostics
[1] int ok = 1;
    -> ok
[2] int d = 1 / 0;
    -> ok
    warning | input_line_2:1:11 | division by zero is undefined
[3] int bad = nope;
    -> failed: Parsing failed.
    error | input_line_3:1:11 | use of undeclared identifier 'nope'
inputs=3 failed=1
```

Division by zero is a warning in this case; the unknown identifier is an error. The valid input after a simple failure demonstrates local recovery, without guaranteeing rollback of every preprocessor or compiler effect.

> [!hint]- Does a rejected multi-declaration input retain its first variable?
> Consider the PTU that owns both declarations.

> [!success]- Follow the failed input
> A simple failed input such as `int a = 1; int b = nope;` has its PTU cleaned up, so `a` is not retained. Header guards and other shared compiler effects can require a fresh session after more complex failures.

Add a deliberate type error followed by a valid declaration to a copied fixture. In the standalone consumer, count errors separately from warnings and report their locations. Compare the counts with API success rather than treating every diagnostic as failure.

---

## Section 4.2 — The code-completion API

Imagine an editor where someone has typed `demo_point.` and is waiting for suggestions. Useful completion depends on declarations already known to the session and on the position being edited, so a list of keywords or a text search cannot answer the whole question.

Use `ReplCodeCompleter` to request suggestions against a prepared interpreter. Compare a name prefix with a member-access expression and inspect the returned candidates. The exercise connects the text being edited to Clang's knowledge of names and types, while keeping completion separate from executing that text.

<!-- runnable-example: 4.2 -->

`examples/p04_complete.cpp`:

```cpp
#include <algorithm>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/CodeCompletion.h>
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

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <string>
)cpp",
    R"cpp(
#include <vector>
)cpp",
    R"cpp(
int demo_counter = 0;
)cpp",
    R"cpp(
int demo_total(int a, int b) {
  return a + b;
}
)cpp",
    R"cpp(
struct Point {
  int x;
  int y;
  int norm2() const {
    return x * x + y * y;
  }
};
)cpp",
    R"cpp(
Point demo_point{3, 4};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<unsigned> Limit("limit",
                                llvm::cl::desc("Maximum completion results"),
                                llvm::cl::cat(Options), llvm::cl::init(8));
  llvm::cl::list<std::string> Prefixes(
      "prefix", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Completion prefix; repeat for several queries"),
      llvm::cl::cat(Options));
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

  std::vector<std::string> Pos(Prefixes.begin(), Prefixes.end());
  if (Pos.empty()) {
    Pos = {"demo_", "demo_point."};
  }

  // The main interpreter: holds the session's declarations.
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
  std::unique_ptr<Interpreter> Main = std::move(*InterpreterOrError);
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = Main->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }

  for (size_t K = 0; K < Pos.size(); ++K) {
    llvm::StringRef Buffer = Pos[K];
    // A fresh CompilerInstance + Interpreter for every request.
    std::vector<const char *> CompilerArguments2;
    for (const std::string &Flag : CompilerFlags) {
      CompilerArguments2.push_back(Flag.c_str());
    }
    clang::IncrementalCompilerBuilder CompilerBuilder2;
    // Give each completion request a separate frontend so its unfinished parse
    // stays isolated.
    CompilerBuilder2.SetCompilerArgs(CompilerArguments2);

    auto CompilerOrError2 = CompilerBuilder2.CreateCpp();
    if (!CompilerOrError2) {
      llvm::logAllUnhandledErrors(CompilerOrError2.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    std::unique_ptr<CompilerInstance> CI = std::move(*CompilerOrError2);
    auto InterpreterOrError2 = Interpreter::create(std::move(CI));
    if (!InterpreterOrError2) {
      llvm::logAllUnhandledErrors(InterpreterOrError2.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    std::unique_ptr<Interpreter> Fresh = std::move(*InterpreterOrError2);

    unsigned Line = std::count(Buffer.begin(), Buffer.end(), '\n') + 1;
    unsigned Col =
        Buffer.size() + 1; // as ClangRepl.cpp: Pos + 1 (single-line buffers)

    ReplCodeCompleter CC;
    std::vector<std::string> Results;
    // Complete Buffer at the 1-based cursor in Fresh, importing declarations
    // from Main.
    // The parent supplies session knowledge; this request does not execute
    // Buffer.
    CC.codeComplete(Fresh->getCompilerInstance(), Buffer, Line, Col,
                    Main->getCompilerInstance(), Results);

    // ClangRepl.cpp keeps the candidates that start with Prefix and inserts
    // only the part after it.
    std::vector<std::string> Matches;
    for (const std::string &R : Results) {
      if (R.find(CC.Prefix) == 0) {
        Matches.push_back(R);
      }
    }
    std::sort(Matches.begin(), Matches.end());
    Matches.erase(std::unique(Matches.begin(), Matches.end()), Matches.end());

    llvm::outs() << "buffer: '" << Buffer << "'\n";
    llvm::outs() << "  Prefix: '" << CC.Prefix
                 << "'  raw results: " << Results.size()
                 << "  matching: " << Matches.size() << "\n";
    for (size_t R = 0; R < Matches.size() && R < Limit; ++R) {
      llvm::outs() << "  " << Matches[R] << "   (inserts '"
                   << Matches[R].substr(CC.Prefix.size()) << "')\n";
    }
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_complete.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_complete
out/p04_complete --prefix=demo_ --prefix=demo_point. -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_complete.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_complete
out/p04_complete --prefix=demo_ --prefix=demo_point. -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

Each request creates a fresh compiler and interpreter for completion, then passes the live session's compiler as `ParentCI` to `codeComplete`. Clang parses the partial buffer up to the cursor in the fresh context and imports visible declarations from the parent. The main session keeps its normal parsing state.

`Line` and `Col` are 1-based. For these single-line buffers, the cursor column is the string position plus one. The returned `Prefix` identifies text already typed, so accepting `lab_total` after `lab_t` should insert only `otal`.

Compare global names and member access:

```bash
cat manifests/p04_complete.repl
build/bin/p04_complete manifests/p04_complete.repl 'lab_' 'lab_t' 'lab_point.' 'lab_point.n'
```

```text expected
// Part 4.10 -- the declarations that completion should know about.
#include <string>
#include <vector>
int lab_counter = 0;
int lab_total(int a, int b) { return a + b; }
struct LabPoint { int x; int y; int norm2() const { return x * x + y * y; } };
LabPoint lab_point{3, 4};
buffer: 'lab_'
  Prefix: 'lab_'  raw results: 3  matching: 3
  lab_counter   (inserts 'counter')
  lab_point   (inserts 'point')
  lab_total   (inserts 'total')
buffer: 'lab_t'
  Prefix: 'lab_t'  raw results: 1  matching: 1
  lab_total   (inserts 'otal')
buffer: 'lab_point.'
  Prefix: ''  raw results: 1  matching: 1
  norm2   (inserts 'norm2')
buffer: 'lab_point.n'
  Prefix: 'n'  raw results: 1  matching: 1
  norm2   (inserts 'orm2')
```

`lab_` offers the session's matching names, while `lab_t` narrows the list. After `lab_point.`, Clang knows the object's type, but LLVM 22.1.8's interpreter completion handler offers methods rather than data fields. `lab_point.n` further filters the method name. The reference tool sorts and deduplicates candidates before display.

Header declarations and keywords can make short prefixes noisy:

```bash
build/bin/p04_complete --limit=5 manifests/p04_complete.repl 'int' 'std::' 'std::s' 'std::vec'
```

```text expected
buffer: 'int'
  Prefix: 'int'  raw results: 15  matching: 15
  int   (inserts '')
  int16_t   (inserts '16_t')
  int32_t   (inserts '32_t')
  int64_t   (inserts '64_t')
  int8_t   (inserts '8_t')
buffer: 'std::'
  Prefix: ''  raw results: 9  matching: 3
  align_val_t   (inserts 'align_val_t')
  nothrow_t   (inserts 'nothrow_t')
  size_t   (inserts 'size_t')
buffer: 'std::s'
  Prefix: 's'  raw results: 3  matching: 1
  size_t   (inserts 'ize_t')
buffer: 'std::vec'
  Prefix: 'vec'  raw results: 0  matching: 0
```

The `int` results include both the keyword and typedefs brought in by headers. The few `std::` names in the recorded run do not represent complete namespace traversal. In this version, the import path brings in top-level declarations and types needed by their signatures; it does not enumerate every namespace member. The listed libc++ names depend on that setup. A host needing broader completion can walk namespace declarations in the main AST itself.

Predict the difference between a partial match and a miss:

```bash
build/bin/p04_complete manifests/p04_complete.repl 'lab_c' 'lab_cx'
```

```text expected
buffer: 'lab_c'
  Prefix: 'lab_c'  raw results: 1  matching: 1
  lab_counter   (inserts 'ounter')
buffer: 'lab_cx'
  Prefix: 'lab_cx'  raw results: 0  matching: 0
```

`lab_c` finds `lab_counter`; `lab_cx` returns an empty list. An empty candidate set is a normal outcome, not a compiler failure.

> [!hint]- Why discard the compiler used for completion?
> Completion stops parsing at the cursor rather than finishing an input.

> [!success]- Follow the two contexts
> The fresh context absorbs the incomplete parse. The parent supplies session declarations without being used as the parser for the partial buffer.

Add `magnitude` to `LabPoint` in a copied fixture and request `lab_point.mag`. Follow the prefix and inserted suffix, then inspect where the request compiler is destroyed. No partial expression needs to execute to produce these suggestions.

---

## Section 4.3 — Name lookup with `Sema`

Your host may need to ask what `counter` denotes before reading or calling anything. Is it a variable, what type does it have, and does a function name refer to several overloads? These are language questions that Clang has already worked out for the session.

Reach the session's `Sema` and perform name lookup, then inspect the declarations it returns. Compare an ordinary name, a qualified namespace name and an overload set. Finding a declaration tells you what the name means; obtaining runtime storage or a callable address is a later, separate operation.

Get the session's `Sema` through its `CompilerInstance`. Build a `LookupResult` for the identifier and ask `LookupName` to search from `TUScope`, the outermost scope.

<!-- runnable-example: 4.3 -->

`examples/p05_lookup.cpp`:

```cpp
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
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
int counter = 41;
)cpp",
    R"cpp(
auto ratio = counter / 4.0;
)cpp",
    R"cpp(
extern "C" int c_add(int a, int b) {
  return a + b;
}
)cpp",
    R"cpp(
int over(int x) {
  return x;
}
)cpp",
    R"cpp(
int over(double x) {
  return (int)x;
}
)cpp",
    R"cpp(
int over(int x, int y) {
  return x + y;
}
)cpp",
    R"cpp(
struct Point {
  int x;
  int y;
};
)cpp",
    R"cpp(
Point origin{3, 4};
)cpp",
    R"cpp(
enum Mode { Fast, Slow = 5 };
)cpp",
    R"cpp(
using Id = long;
)cpp",
    R"cpp(
namespace geo {
int unit = 10;
struct Box {
  int w;
  int h;
};
double area(double w, double h) {
  return w * h;
}
namespace inner {
int deep = 3;
}
} // namespace geo
)cpp",
    R"cpp(
template <typename T> T twice(T x) {
  return x + x;
}
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: lookup, qualified, value, decls"),
      llvm::cl::cat(Options), llvm::cl::init("lookup"));
  llvm::cl::list<std::string> NamesOption(
      "name", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
  llvm::cl::opt<std::string> Scope(
      "scope", llvm::cl::desc("Namespace path used by qualified lookup"),
      llvm::cl::cat(Options), llvm::cl::init("geo::inner"));
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

  std::vector<std::string> Names(NamesOption.begin(), NamesOption.end());
  if (Names.empty()) {
    Names = {"counter", "ratio", "over", "Point"};
  }
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
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }

  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();

  if (Mode == "lookup") {
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      // LookupResult can contain a single declaration, an overload set or no
      // match.
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      // Use the live Sema and TU scope to apply the session's C++ name-lookup
      // rules.
      S.LookupName(R, S.TUScope);
      {
        auto &&IArgument = (*I);
        auto &&SArgument = (S);
        auto &&LabelArgument = (Name);
        auto &&RArgument = (R);
        llvm::outs() << LabelArgument << ": ";
        if (RArgument.empty()) {
          llvm::outs() << "not found\n";
        } else {
          // An overload name is a set of candidates, not an already selected
          // callable.
          if (RArgument.isOverloadedResult()) {
            llvm::outs() << "overload set of "
                         << RArgument.end() - RArgument.begin() << "\n";
          }
          for (clang::NamedDecl *Declaration : RArgument) {
            llvm::outs() << Declaration->getDeclKindName() << "Decl "
                         << Declaration->getQualifiedNameAsString();
            if (auto *Value = llvm::dyn_cast<clang::ValueDecl>(Declaration)) {
              llvm::outs() << R"cpp(: type ')cpp"
                           << Value->getType().getAsString(
                                  IArgument.getASTContext().getPrintingPolicy())
                           << R"cpp(')cpp";
            }
            llvm::outs() << "\n";
          }
        }
      }
    }
  } else if (Mode == "qualified") {
    // Walk a scope path such as geo::inner, one qualified lookup per step,
    // starting at the translation unit.
    DeclContext *DC = C.getTranslationUnitDecl();
    llvm::SmallVector<llvm::StringRef, 4> Path;
    llvm::StringRef(Scope).split(Path, "::");
    for (llvm::StringRef Step : Path) {
      LookupResult R(S, DeclarationName(&C.Idents.get(Step)), SourceLocation(),
                     Sema::LookupNamespaceName);
      // Resolve one name within this DeclContext; descend through namespaces
      // one scope at a time.
      S.LookupQualifiedName(R, DC);
      auto *NS =
          R.empty() ? nullptr : llvm::dyn_cast<NamespaceDecl>(R.getFoundDecl());
      if (!NS) {
        llvm::outs() << "no namespace " << Step << "\n";
        return 1;
      }
      DC = NS;
    }
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      S.LookupQualifiedName(R, DC);
      {
        auto &&IArgument2 = (*I);
        auto &&SArgument2 = (S);
        auto &&LabelArgument2 = (Scope + "::" + Name);
        auto &&RArgument2 = (R);
        llvm::outs() << LabelArgument2 << ": ";
        if (RArgument2.empty()) {
          llvm::outs() << "not found\n";
        } else {
          if (RArgument2.isOverloadedResult()) {
            llvm::outs() << "overload set of "
                         << RArgument2.end() - RArgument2.begin() << "\n";
          }
          for (clang::NamedDecl *Declaration : RArgument2) {
            llvm::outs() << Declaration->getDeclKindName() << "Decl "
                         << Declaration->getQualifiedNameAsString();
            if (auto *Value = llvm::dyn_cast<clang::ValueDecl>(Declaration)) {
              llvm::outs()
                  << R"cpp(: type ')cpp"
                  << Value->getType().getAsString(
                         IArgument2.getASTContext().getPrintingPolicy())
                  << R"cpp(')cpp";
            }
            llvm::outs() << "\n";
          }
        }
      }
    }
  } else if (Mode == "value") {
    for (size_t K = 0; K < Names.size(); ++K) {
      const std::string &Name = Names[K];
      LookupResult R(S, DeclarationName(&C.Idents.get(Name)), SourceLocation(),
                     Sema::LookupOrdinaryName);
      S.LookupName(R, S.TUScope);
      auto *VD =
          R.empty() ? nullptr : llvm::dyn_cast<VarDecl>(R.getFoundDecl());
      if (!VD) {
        llvm::outs() << Name << ": not a variable\n";
        continue;
      }
      // The address of the variable's storage in the JIT: no value printing
      // involved.
      // Resolve a runtime address; lookup may compile/link a module on demand.
      // The session owns code/storage; function-pointer types must match the
      // ABI.
      auto AddressOrError = I->getSymbolAddress(GlobalDecl(VD));
      if (!AddressOrError) {
        llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *P = (*AddressOrError).toPtr<void *>();
      QualType T = VD->getType();
      std::string AddressText;
      llvm::raw_string_ostream OutputForAddressText(AddressText);
      OutputForAddressText << llvm::format(
          "0x%llxUL",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>((P))));
      llvm::outs() << Name << ": '" << (T.getAsString(C.getPrintingPolicy()))
                   << "' at " << (AddressText) << ", "
                   << C.getTypeSizeInChars(T).getQuantity() << " bytes";
      if (const auto *BT = T->getAs<BuiltinType>()) {
        if (BT->getKind() == BuiltinType::Int) {
          llvm::outs() << ", value " << *static_cast<int *>(P);
        } else if (BT->getKind() == BuiltinType::Double) {
          llvm::outs() << ", value "
                       << llvm::format("%g", *static_cast<double *>(P));
        }
      }
      llvm::outs() << "\n";
      if (Name ==
          "counter") { // write through the address; the interpreter sees it
        *static_cast<int *>(P) = 1000;
        if (llvm::Error Error = I->ParseAndExecute(R"cpp(
counter
)cpp")) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
      }
    }
  } else if (Mode == "decls") {
    SourceManager &SM = C.getSourceManager();
    TranslationUnitDecl *TU = C.getTranslationUnitDecl();
    llvm::outs() << "getTranslationUnitDecl()->decls(): "
                 << std::distance(TU->decls_begin(), TU->decls_end())
                 << " declaration(s): only the newest input\n";

    // Every input is its own TranslationUnitDecl, chained as redeclarations,
    // newest first. input_line_0 is the interpreter's own runtime prelude.
    std::vector<TranslationUnitDecl *> Parts(TU->redecls().begin(),
                                             TU->redecls().end());
    llvm::outs() << "redecls(): " << Parts.size() << " parts\n";
    std::function<void(Decl *)> show = [&](Decl *D) {
      llvm::StringRef File =
          SM.getFilename(SM.getSpellingLoc(D->getLocation()));
      if (D->isImplicit() || !File.starts_with("input_line_") ||
          File == "input_line_0") {
        return;
      }
      if (auto *LS = llvm::dyn_cast<LinkageSpecDecl>(D)) { // extern "C" { ... }
        for (Decl *Inner : LS->decls()) {
          show(Inner);
        }
      } else if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        std::string DeclarationText;

        ASTContext &DeclarationContext = (*I).getASTContext();
        llvm::raw_string_ostream DeclarationOutput(DeclarationText);
        DeclarationOutput << ND->getDeclKindName() << "Decl";
        if (auto *VD = llvm::dyn_cast<VarDecl>((ND))) {
          DeclarationOutput
              << ": type '"
              << ((VD->getType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
          if (VD->hasInit()) {
            std::string InitializerText;
            llvm::raw_string_ostream InitializerOutput(InitializerText);
            (VD->getInit())
                ->printPretty(InitializerOutput, nullptr,
                              (DeclarationContext).getPrintingPolicy());
            DeclarationOutput << ", init '" << InitializerText << "'";
          }
        } else if (auto *FD = llvm::dyn_cast<FunctionDecl>((ND))) {
          DeclarationOutput
              << ": type '"
              << ((FD->getType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
          if (FD->isExternC()) {
            DeclarationOutput << ", extern \"C\"";
          }
        } else if (auto *TD = llvm::dyn_cast<TypedefNameDecl>((ND))) {
          DeclarationOutput
              << ": '" << ND->getName() << "' names '"
              << ((TD->getUnderlyingType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "', canonical '"
              << ((TD->getUnderlyingType().getCanonicalType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
        } else if (auto *RD = llvm::dyn_cast<CXXRecordDecl>((ND))) {
          DeclarationOutput
              << ": " << (RD->hasDefinition() ? "defined" : "declared only")
              << ", " << std::distance(RD->field_begin(), RD->field_end())
              << " fields";
        } else if (auto *ED = llvm::dyn_cast<EnumDecl>((ND))) {
          DeclarationOutput
              << ": underlying '"
              << ((ED->getIntegerType())
                      .getAsString((DeclarationContext).getPrintingPolicy()))
              << "'";
        } else if (auto *EC = llvm::dyn_cast<EnumConstantDecl>((ND))) {
          DeclarationOutput << ": value " << EC->getInitVal() << " of enum '"
                            << EC->getType().getAsString() << "'";
        } else if (auto *FT = llvm::dyn_cast<FunctionTemplateDecl>((ND))) {
          DeclarationOutput << ": " << FT->getTemplateParameters()->size()
                            << " template parameter(s), "
                            << std::distance(FT->specializations().begin(),
                                             FT->specializations().end())
                            << " specialization(s)";
        } else if (auto *NS = llvm::dyn_cast<NamespaceDecl>((ND))) {
          DeclarationOutput << ": '" << NS->getQualifiedNameAsString() << "'";
        }

        llvm::outs() << "  " << File << "  " << ND->getNameAsString() << "  "
                     << DeclarationText << "\n";
      }
    };
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It) {
      for (Decl *D : (*It)->decls()) {
        show(D);
      }
    }

    // The PTU that Parse() returns is the newest part.
    // Build the input's AST and IR without running its initializers.
    auto ParsedInputOrError = I->Parse(R"cpp(
int late = 1;
)cpp");
    if (!ParsedInputOrError) {
      llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    PartialTranslationUnit &PTU = (*ParsedInputOrError);
    llvm::outs() << "Parse(): PTU.TUPart is the new TU: "
                 << (PTU.TUPart == C.getTranslationUnitDecl()) << ", with "
                 << std::distance(PTU.TUPart->decls_begin(),
                                  PTU.TUPart->decls_end())
                 << " declaration(s)\n";
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_lookup.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p05_lookup
out/p05_lookup --mode=lookup --name=counter --name=ratio --name=over --name=Point -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_lookup.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p05_lookup
out/p05_lookup --mode=lookup --name=counter --name=ratio --name=over --name=Point -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

For qualified names, use `LookupQualifiedName` with the containing `DeclContext`. The reference fixture gives lookup several kinds of declarations to find:

```bash
cat manifests/p05_lookup.repl
```

```text expected
// Part 5.7 -- one of everything a lookup can find
int counter = 41;
auto ratio = counter / 4.0;
extern "C" int c_add(int a, int b) { return a + b; }
int over(int x) { return x; }
int over(double x) { return (int)x; }
int over(int x, int y) { return x + y; }
struct Point { int x; int y; };
Point origin{3, 4};
enum Mode { Fast, Slow = 5 };
using Id = long;
namespace geo { \
  int unit = 10; \
  struct Box { int w; int h; }; \
  double area(double w, double h) { return w * h; } \
  namespace inner { int deep = 3; } \
}
template <typename T> T twice(T x) { return x + x; }
```

Predict which names identify a single declaration and which identify candidates:

```bash
build/bin/p05_lookup manifests/p05_lookup.repl lookup counter ratio c_add over Point origin Mode Fast Slow Id geo twice nosuch printf
```

```text expected
counter: VarDecl: type 'int', init '41'
ratio: VarDecl: type 'double', init 'counter / 4.'
c_add: FunctionDecl: type 'int (int, int)', extern "C"
over: overload set of 3
    FunctionDecl: type 'int (int)'
    FunctionDecl: type 'int (double)'
    FunctionDecl: type 'int (int, int)'
Point: CXXRecordDecl: defined, 2 fields
origin: VarDecl: type 'Point', init '{3, 4}'
Mode: EnumDecl: underlying 'unsigned int'
Fast: EnumConstantDecl: value 0 of enum 'enum Mode'
Slow: EnumConstantDecl: value 5 of enum 'enum Mode'
Id: TypeAliasDecl: 'Id' names 'long', canonical 'long'
geo: NamespaceDecl: 'geo'
twice: overload set of 1
    FunctionTemplateDecl: 1 template parameter(s), 0 specialization(s)
nosuch: not found
printf: not found
```

A `VarDecl` exposes the declared type and initializer expression. `auto` has already been deduced, but `getInit()` still describes source computation rather than the current stored value. A function name can produce several declarations; inspect `isOverloadedResult()` and iterate instead of calling a single-result accessor. A function template can also appear in an overload result before any specialization exists.

Types, aliases, enumerators and namespaces are declarations too. An empty result means the session has no visible declaration for that query; a host include alone cannot make `printf` visible in the session.

Qualified lookup follows one context at a time, from `geo` to `inner` to `deep`:

```bash
build/bin/p05_lookup manifests/p05_lookup.repl qualified geo unit Box area inner nosuch
build/bin/p05_lookup manifests/p05_lookup.repl qualified geo::inner deep
```

```text expected
geo::unit: VarDecl: type 'int', init '10'
geo::Box: CXXRecordDecl: defined, 2 fields
geo::area: FunctionDecl: type 'double (double, double)'
geo::inner: NamespaceDecl: 'geo::inner'
geo::nosuch: not found
geo::inner::deep: VarDecl: type 'int', init '3'
```

To read a variable's current value, cross from compiler metadata to runtime storage:

```bash
build/bin/p05_lookup manifests/p05_lookup.repl value counter ratio origin c_add
```

```text expected
counter: 'int' at 0xADDR, 4 bytes, value 41
(int) 1000
ratio: 'double' at 0xADDR, 8 bytes, value 10.25
origin: 'Point' at 0xADDR, 8 bytes
c_add: not a variable
```

`getSymbolAddress(GlobalDecl(VD))` resolves the variable without guessing its mangled name. The host reads `counter` as an `int` and `ratio` as a `double`, using their types to interpret the bytes. Writing 1000 through `counter`'s address changes what the JIT sees. A class object requires layout-aware access rather than an arbitrary scalar cast.

Listing the whole session needs the translation-unit redeclaration chain:

```bash
build/bin/p05_lookup manifests/p05_lookup.repl decls
```

```text expected
getTranslationUnitDecl()->decls(): 1 declaration(s): only the newest input
redecls(): 15 parts
  input_line_2  counter  VarDecl: type 'int', init '41'
  input_line_3  ratio  VarDecl: type 'double', init 'counter / 4.'
  input_line_4  c_add  FunctionDecl: type 'int (int, int)', extern "C"
  input_line_5  over  FunctionDecl: type 'int (int)'
  input_line_6  over  FunctionDecl: type 'int (double)'
  input_line_7  over  FunctionDecl: type 'int (int, int)'
  input_line_8  Point  CXXRecordDecl: defined, 2 fields
  input_line_9  origin  VarDecl: type 'Point', init '{3, 4}'
  input_line_10  Mode  EnumDecl: underlying 'unsigned int'
  input_line_11  Id  TypeAliasDecl: 'Id' names 'long', canonical 'long'
  input_line_12  geo  NamespaceDecl: 'geo'
  input_line_13  twice  FunctionTemplateDecl: 1 template parameter(s), 0 specialization(s)
Parse(): PTU.TUPart is the new TU: 1, with 1 declaration(s)
```

The latest TU's `decls()` covers only the latest input. Walking `redecls()` reaches earlier parts; the reference tool filters the startup prelude and descends through `LinkageSpecDecl` wrappers for `extern "C"` declarations.

Predict the result for two ordinary overloads and a template:

```bash
mkdir -p out
printf '%s\n' 'int add(int a, int b) { return a + b; }' 'double add(double a, double b) { return a + b; }' 'template <typename T> T add(T a, T b, T c) { return a + b + c; }' > out/p05_verify_lookup.repl
build/bin/p05_lookup out/p05_verify_lookup.repl lookup add
```

```text expected
add: overload set of 3
    FunctionTemplateDecl: 1 template parameter(s), 0 specialization(s)
    FunctionDecl: type 'int (int, int)'
    FunctionDecl: type 'double (double, double)'
```

One lookup returns three candidates. Selecting one for a call requires its parameter types and the intended call, rather than the order of declarations in the result.

> [!hint]- Can a cached declaration pointer survive undo?
> Its owner is the compiler session, and undo changes that session.

> [!success]- Follow the declaration's lifetime
> Treat a declaration from a removed input as invalid. Look it up again after removal or session replacement.

Add another `over` overload to a copied fixture and enumerate every candidate. Explain which properties describe the declaration and which operation would be needed to obtain executable code.

---

## Section 4.4 — Walking types and record layouts

Suppose the JIT creates a record whose C++ definition is not compiled into the host. Reading its fields requires more than casting an address: the host must discover field types, offsets, bases and any padding chosen by the target's layout rules.

Ask `ASTContext` for that information and follow a type from its written spelling to its underlying form. Compare a simple record with nested fields, inheritance and bit-fields. Use the compiler's answers to explain where each member lives instead of guessing from the order of declarations or a layout seen on another machine.

A `QualType` can retain the spelling a user wrote while referring to an underlying canonical type. Choose the view according to the question: readable spelling for display, semantic identity for comparisons.

| Call | Result |
|------|--------|
| `T.getAsString(Policy)` | the type as written (`CId`) |
| `T.getSingleStepDesugaredType(C)` | one layer off (`const Id`) |
| `T.getDesugaredType(C)` | all *top-level* sugar off, but not inside (`const CId *` keeps `CId`) |
| `T.getCanonicalType()` | the unique canonical form, sugar gone everywhere (`const long *`) |

<!-- runnable-example: 4.4 -->

`examples/p05_layout.cpp`:

```cpp
#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// ---- 5.8: layout of one record --------------------------------------------

struct Region {
  uint64_t Bit, Bits;
  std::string What;
};

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <cstddef>
)cpp",
    R"cpp(
#include <string>
)cpp",
    R"cpp(
using Id = long;
)cpp",
    R"cpp(
typedef const Id CId;
)cpp",
    R"cpp(
CId answer = 42;
)cpp",
    R"cpp(
const CId *pid = &answer;
)cpp",
    R"cpp(
std::size_t count = 3;
)cpp",
    R"cpp(
std::string text = "layout";
)cpp",
    R"cpp(
auto deduced = pid;
)cpp",
    R"cpp(
struct Inner {
  char c;
  double d;
};
)cpp",
    R"cpp(
struct Base {
  int b;
};
)cpp",
    R"cpp(
struct Layout : Base {
  char tag;
  short s;
  Inner in;
  int arr[3];
  bool flag;
  void *p;
};
)cpp",
    R"cpp(
struct Poly {
  virtual ~Poly() {
  }
  int v;
};
)cpp",
    R"cpp(
struct Bits {
  unsigned a : 3;
  unsigned b : 5;
  int c;
};
)cpp",
    R"cpp(
struct VB {
  int v;
};
)cpp",
    R"cpp(
struct VD : virtual VB {
  int d;
};
)cpp",
    R"cpp(
union U {
  char c;
  double d;
  int i[3];
};
)cpp",
    R"cpp(
template <typename T> struct Pair {
  T first;
  char tag;
  T second;
};
)cpp",
    R"cpp(
Pair<int> pi;
)cpp",
    R"cpp(
Pair<double> pd;
)cpp",
    R"cpp(
Pair<char> pc;
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: types, layout, template"),
      llvm::cl::cat(Options), llvm::cl::init("layout"));
  llvm::cl::list<std::string> NamesOption(
      "name", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
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

  std::vector<std::string> Names(NamesOption.begin(), NamesOption.end());
  if (Names.empty()) {
    Names = {"Inner", "Layout"};
  }
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
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();

  for (size_t K = 0; K < Names.size(); ++K) {
    clang::NamedDecl *FoundDeclaration;
    clang::ASTContext &LookupContext = S.getASTContext();
    clang::LookupResult Lookup(
        (S), clang::DeclarationName(&LookupContext.Idents.get((Names[K]))),
        clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
    S.LookupName(Lookup, S.TUScope);
    FoundDeclaration =
        Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
    NamedDecl *ND = FoundDeclaration;
    if (!ND) {
      llvm::outs() << Names[K] << ": not found\n";
      continue;
    }
    if (Mode == "types") {
      auto *VD = llvm::dyn_cast<VarDecl>(ND);
      if (!VD) {
        llvm::outs() << Names[K] << ": not a variable\n";
        continue;
      }
      // Keep readable spelling for display; canonical types remove aliases for
      // semantic comparison.
      QualType T = VD->getType();
      llvm::outs() << Names[K] << ":\n";
      llvm::outs() << "  as written          "
                   << (T.getAsString(C.getPrintingPolicy())) << "\n";
      llvm::outs() << "  one step desugared  "
                   << ((T.getSingleStepDesugaredType(C))
                           .getAsString(C.getPrintingPolicy()))
                   << "\n";
      llvm::outs()
          << "  desugared           "
          << ((T.getDesugaredType(C)).getAsString(C.getPrintingPolicy()))
          << "\n";
      llvm::outs()
          << "  canonical           "
          << ((T.getCanonicalType()).getAsString(C.getPrintingPolicy()))
          << "\n";
      llvm::outs() << "  class of the type   " << T->getTypeClassName()
                   << " -> " << T.getCanonicalType()->getTypeClassName()
                   << "\n";
    } else if (Mode == "layout") {
      auto *RD = llvm::dyn_cast<CXXRecordDecl>(ND);
      if (!RD || !RD->getDefinition()) {
        llvm::outs() << Names[K] << ": not a complete class\n";
        continue;
      }
      {
        clang::ASTContext &CArgument = (C);
        const clang::CXXRecordDecl *RDArgument = (RD);

        RDArgument = RDArgument->getDefinition();
        // Ask the current target's ASTContext for a complete record's actual
        // storage layout.
        const ASTRecordLayout &L = CArgument.getASTRecordLayout(RDArgument);
        llvm::outs() << (RDArgument->isUnion() ? "union " : "struct ")
                     << (clang::QualType(
                             CArgument.getCanonicalTagType(RDArgument))
                             .getAsString((CArgument).getPrintingPolicy()))
                     << ": size " << L.getSize().getQuantity() << ", align "
                     << L.getAlignment().getQuantity() << ", data size "
                     << L.getDataSize().getQuantity() << ", non-virtual size "
                     << L.getNonVirtualSize().getQuantity() << "\n";

        std::vector<Region> Regions;
        if (RDArgument->isDynamicClass() && L.hasOwnVFPtr()) {
          Regions.push_back(
              {0, CArgument.getTypeSize(CArgument.VoidPtrTy), "<vptr>"});
        }
        for (const CXXBaseSpecifier &B : RDArgument->bases()) {
          const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
          // A base's data size, not its size: the derived class may reuse its
          // tail padding. A virtual base lives wherever the most-derived class
          // puts it.
          CharUnits BOff = B.isVirtual() ? L.getVBaseClassOffset(BRD)
                                         : L.getBaseClassOffset(BRD);
          Regions.push_back(
              {(uint64_t)BOff.getQuantity() * 8,
               (uint64_t)CArgument.getASTRecordLayout(BRD)
                       .getDataSize()
                       .getQuantity() *
                   8,
               std::string(B.isVirtual() ? "virtual base " : "base ") +
                   BRD->getNameAsString()});
        }
        unsigned I = 0;
        for (const FieldDecl *F : RDArgument->fields()) {
          uint64_t Bits = F->isBitField() ? F->getBitWidthValue()
                                          : CArgument.getTypeSize(F->getType());
          std::string What =
              F->getNameAsString() + " : " +
              ((F->getType()).getAsString((CArgument).getPrintingPolicy()));
          if (F->isBitField()) {
            What += " : " + std::to_string(Bits);
          }
          // Field offsets are bits, unlike CharUnits sizes and base offsets;
          // retain those units here.
          Regions.push_back({L.getFieldOffset(I++), Bits, What});
        }
        std::stable_sort(
            Regions.begin(), Regions.end(),
            [](const Region &A, const Region &B) { return A.Bit < B.Bit; });

        llvm::outs() << "  offset  bits  what\n";
        uint64_t Cursor = 0;

        for (const Region &R : Regions) {
          if (R.Bit > Cursor) {
            {
              auto &&BitArgument = (Cursor);
              auto &&BitsArgument = (R.Bit - Cursor);
              auto &&WhatArgument = ("<padding>");

              llvm::outs() << llvm::format("  %6s  %4llu  ",
                                           (std::to_string((BitArgument) / 8) +
                                            (((BitArgument) % 8)
                                                 ? "." + std::to_string(
                                                             (BitArgument) % 8)
                                                 : ""))
                                               .c_str(),
                                           (unsigned long long)BitsArgument)
                           << WhatArgument << "\n";
            }
          }
          {
            auto &&BitArgument2 = (R.Bit);
            auto &&BitsArgument2 = (R.Bits);
            auto &&WhatArgument2 = (R.What);

            llvm::outs() << llvm::format(
                                "  %6s  %4llu  ",
                                (std::to_string((BitArgument2) / 8) +
                                 (((BitArgument2) % 8)
                                      ? "." + std::to_string((BitArgument2) % 8)
                                      : ""))
                                    .c_str(),
                                (unsigned long long)BitsArgument2)
                         << WhatArgument2 << "\n";
          }
          Cursor =
              std::max(Cursor,
                       R.Bit + R.Bits); // unions overlap: keep the furthest end
        }
        uint64_t End = (uint64_t)L.getSize().getQuantity() * 8;
        if (Cursor < End) {
          {
            auto &&BitArgument3 = (Cursor);
            auto &&BitsArgument3 = (End - Cursor);
            auto &&WhatArgument3 = ("<tail padding>");

            llvm::outs() << llvm::format(
                                "  %6s  %4llu  ",
                                (std::to_string((BitArgument3) / 8) +
                                 (((BitArgument3) % 8)
                                      ? "." + std::to_string((BitArgument3) % 8)
                                      : ""))
                                    .c_str(),
                                (unsigned long long)BitsArgument3)
                         << WhatArgument3 << "\n";
          }
        }
      }
    } else if (Mode == "template") {
      auto *CT = llvm::dyn_cast<ClassTemplateDecl>(ND);
      if (!CT) {
        llvm::outs() << Names[K] << ": not a class template\n";
        continue;
      }
      std::vector<ClassTemplateSpecializationDecl *> Specs(
          CT->specializations().begin(), CT->specializations().end());
      std::sort(Specs.begin(), Specs.end(), [&](auto *X, auto *Y) {
        return (clang::QualType(C.getCanonicalTagType(X))
                    .getAsString(C.getPrintingPolicy())) <
               (clang::QualType(C.getCanonicalTagType(Y))
                    .getAsString(C.getPrintingPolicy()));
      });
      for (ClassTemplateSpecializationDecl *Spec : Specs) {
        if (Spec->isCompleteDefinition()) {
          {
            clang::ASTContext &CArgument2 = (C);
            const clang::CXXRecordDecl *RDArgument2 = (Spec);

            RDArgument2 = RDArgument2->getDefinition();
            const ASTRecordLayout &L =
                CArgument2.getASTRecordLayout(RDArgument2);
            llvm::outs() << (RDArgument2->isUnion() ? "union " : "struct ")
                         << (clang::QualType(
                                 CArgument2.getCanonicalTagType(RDArgument2))
                                 .getAsString((CArgument2).getPrintingPolicy()))
                         << ": size " << L.getSize().getQuantity() << ", align "
                         << L.getAlignment().getQuantity() << ", data size "
                         << L.getDataSize().getQuantity()
                         << ", non-virtual size "
                         << L.getNonVirtualSize().getQuantity() << "\n";

            std::vector<Region> Regions;
            if (RDArgument2->isDynamicClass() && L.hasOwnVFPtr()) {
              Regions.push_back(
                  {0, CArgument2.getTypeSize(CArgument2.VoidPtrTy), "<vptr>"});
            }
            for (const CXXBaseSpecifier &B : RDArgument2->bases()) {
              const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
              // A base's data size, not its size: the derived class may reuse
              // its tail padding. A virtual base lives wherever the
              // most-derived class puts it.
              CharUnits BOff = B.isVirtual() ? L.getVBaseClassOffset(BRD)
                                             : L.getBaseClassOffset(BRD);
              Regions.push_back(
                  {(uint64_t)BOff.getQuantity() * 8,
                   (uint64_t)CArgument2.getASTRecordLayout(BRD)
                           .getDataSize()
                           .getQuantity() *
                       8,
                   std::string(B.isVirtual() ? "virtual base " : "base ") +
                       BRD->getNameAsString()});
            }
            unsigned I = 0;
            for (const FieldDecl *F : RDArgument2->fields()) {
              uint64_t Bits = F->isBitField()
                                  ? F->getBitWidthValue()
                                  : CArgument2.getTypeSize(F->getType());
              std::string What =
                  F->getNameAsString() + " : " +
                  ((F->getType())
                       .getAsString((CArgument2).getPrintingPolicy()));
              if (F->isBitField()) {
                What += " : " + std::to_string(Bits);
              }
              Regions.push_back({L.getFieldOffset(I++), Bits, What});
            }
            std::stable_sort(
                Regions.begin(), Regions.end(),
                [](const Region &A, const Region &B) { return A.Bit < B.Bit; });

            llvm::outs() << "  offset  bits  what\n";
            uint64_t Cursor = 0;
            auto row =
                [&](uint64_t Bit, uint64_t Bits, const std::string &What) {
                  llvm::outs()
                      << llvm::format(
                             "  %6s  %4llu  ",
                             (std::to_string((Bit) / 8) +
                              (((Bit) % 8) ? "." + std::to_string((Bit) % 8)
                                           : ""))
                                 .c_str(),
                             (unsigned long long)Bits)
                      << What << "\n";
                };
            for (const Region &R : Regions) {
              if (R.Bit > Cursor) {
                {
                  auto &&BitArgument4 = (Cursor);
                  auto &&BitsArgument4 = (R.Bit - Cursor);
                  auto &&WhatArgument4 = ("<padding>");

                  llvm::outs()
                      << llvm::format(
                             "  %6s  %4llu  ",
                             (std::to_string((BitArgument4) / 8) +
                              (((BitArgument4) % 8)
                                   ? "." + std::to_string((BitArgument4) % 8)
                                   : ""))
                                 .c_str(),
                             (unsigned long long)BitsArgument4)
                      << WhatArgument4 << "\n";
                }
              }
              {
                auto &&BitArgument5 = (R.Bit);
                auto &&BitsArgument5 = (R.Bits);
                auto &&WhatArgument5 = (R.What);

                llvm::outs()
                    << llvm::format(
                           "  %6s  %4llu  ",
                           (std::to_string((BitArgument5) / 8) +
                            (((BitArgument5) % 8)
                                 ? "." + std::to_string((BitArgument5) % 8)
                                 : ""))
                               .c_str(),
                           (unsigned long long)BitsArgument5)
                    << WhatArgument5 << "\n";
              }
              Cursor = std::max(
                  Cursor,
                  R.Bit + R.Bits); // unions overlap: keep the furthest end
            }
            uint64_t End = (uint64_t)L.getSize().getQuantity() * 8;
            if (Cursor < End) {
              {
                auto &&BitArgument6 = (Cursor);
                auto &&BitsArgument6 = (End - Cursor);
                auto &&WhatArgument6 = ("<tail padding>");

                llvm::outs()
                    << llvm::format(
                           "  %6s  %4llu  ",
                           (std::to_string((BitArgument6) / 8) +
                            (((BitArgument6) % 8)
                                 ? "." + std::to_string((BitArgument6) % 8)
                                 : ""))
                               .c_str(),
                           (unsigned long long)BitsArgument6)
                    << WhatArgument6 << "\n";
              }
            }
          }
        }
      }
    }
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_layout.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p05_layout
out/p05_layout --mode=layout --name=Inner --name=Layout -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_layout.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p05_layout
out/p05_layout --mode=layout --name=Inner --name=Layout -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

Compare aliases, deduced types and canonical forms:

```bash
build/bin/p05_layout manifests/p05_layout.repl types answer pid count text deduced
```

```text expected
answer:
  as written          CId
  one step desugared  const Id
  desugared           const long
  canonical           const long
  class of the type   Typedef -> Builtin
pid:
  as written          const CId *
  one step desugared  const CId *
  desugared           const CId *
  canonical           const long *
  class of the type   Pointer -> Pointer
count:
  as written          std::size_t
  one step desugared  decltype(sizeof(int))
  desugared           unsigned long
  canonical           unsigned long
  class of the type   Typedef -> Builtin
text:
  as written          std::string
  one step desugared  basic_string<char>
  desugared           std::string
  canonical           std::string
  class of the type   Typedef -> Record
deduced:
  as written          const CId *
  one step desugared  const CId *
  desugared           const CId *
  canonical           const long *
  class of the type   Auto -> Pointer
```

Desugaring the outer layer does not necessarily desugar types nested inside a pointer. Canonicalization removes those distinctions for semantic comparisons. Use `ASTContext::hasSameType` or canonical `QualType`s instead of comparing printed strings; the type printer can abbreviate library types.

For a complete record, ask `ASTContext::getASTRecordLayout` how the target places its storage:

| Call | Meaning |
|------|---------|
| `getSize()`, `getAlignment()` | `sizeof` and `alignof`, as `CharUnits` (bytes) |
| `getDataSize()` | where the next field could go: the size without tail padding |
| `getNonVirtualSize()` | size ignoring virtual bases |
| `getFieldOffset(i)` | offset in **bits** of the *i*-th field in declaration order |
| `getBaseClassOffset(Base)` | offset in bytes of a non-virtual base subobject |
| `hasOwnVFPtr()` | this class introduces the vtable pointer (at offset 0) |

Keep the units visible: field offsets are bits, whereas `CharUnits` sizes and base offsets are target character units. The reference tool places bases, fields and implementation regions in order to display the gaps:

```bash
build/bin/p05_layout manifests/p05_layout.repl layout Inner Layout Poly VD Bits U
```

```text expected
struct Inner: size 16, align 8, data size 16, non-virtual size 16
  offset  bits  what
       0     8  c : char
       1    56  <padding>
       8    64  d : double
struct Layout: size 48, align 8, data size 48, non-virtual size 48
  offset  bits  what
       0    32  base Base
       4     8  tag : char
       5     8  <padding>
       6    16  s : short
       8   128  in : Inner
      24    96  arr : int[3]
      36     8  flag : bool
      37    24  <padding>
      40    64  p : void *
struct Poly: size 16, align 8, data size 12, non-virtual size 12
  offset  bits  what
       0    64  <vptr>
       8    32  v : int
      12    32  <tail padding>
struct VD: size 16, align 8, data size 16, non-virtual size 12
  offset  bits  what
       0    64  <vptr>
       8    32  d : int
      12    32  virtual base VB
struct Bits: size 8, align 4, data size 8, non-virtual size 8
  offset  bits  what
       0     3  a : unsigned int : 3
     0.3     5  b : unsigned int : 5
       1    24  <padding>
       4    32  c : int
union U: size 16, align 8, data size 16, non-virtual size 16
  offset  bits  what
       0     8  c : char
       0    64  d : double
       0    96  i : int[3]
      12    32  <tail padding>
```

`Inner` shows padding before an aligned `double`. `Poly` shows a virtual-table pointer and tail padding; that pointer is not an ordinary field. `VD` needs virtual-base offsets from the complete class's layout, rather than the non-virtual-base accessor. `Bits` needs bit offsets and widths, while the union's fields overlap at zero.

A class template acquires a concrete layout after specialization:

```bash
build/bin/p05_layout manifests/p05_layout.repl template Pair
```

```text expected
struct Pair<char>: size 3, align 1, data size 3, non-virtual size 3
  offset  bits  what
       0     8  first : char
       1     8  tag : char
       2     8  second : char
struct Pair<double>: size 24, align 8, data size 24, non-virtual size 24
  offset  bits  what
       0    64  first : double
       8     8  tag : char
       9    56  <padding>
      16    64  second : double
struct Pair<int>: size 12, align 4, data size 12, non-virtual size 12
  offset  bits  what
       0    32  first : int
       4     8  tag : char
       5    24  <padding>
       8    32  second : int
```

`Pair<char>`, `Pair<int>` and `Pair<double>` share source structure but have different padding. `ClassTemplateDecl::specializations()` reaches their concrete record declarations.

Predict `sizeof(Inner)`, then compare the AST's answer with the generated computation:

```bash
build/bin/p05_layout manifests/p05_layout.repl layout Inner | head -1
build/bin/p04_hello 'struct Inner { char c; double d; };' 'sizeof(Inner)' | tail -1
```

```text expected
struct Inner: size 16, align 8, data size 16, non-virtual size 16
(unsigned long) 16
```

Both answers come from the same target layout. The recorded sizes describe that target, so repeat the query when the architecture, ABI or library changes.

> [!hint]- Why distinguish a base's data size from its full size?
> Tail padding can participate in a derived layout.

> [!success]- Follow the base regions
> The tested Itanium layout can reuse tail padding of a suitable base. Use the actual base and field offsets from `ASTRecordLayout` rather than placing every derived field after `sizeof(Base)`.

Reorder `char` and `double` in a copied definition and predict the new padding. Then add a bit-field. Explain why dividing its bit offset by eight does not produce an ordinary pointer to that field.

---

## Section 4.5 — Templates on demand

A function template describes a family of functions. Your host might learn at runtime that it needs `scale<double>`, even though that specialization was never selected when the host itself was built.

Make the interpreter instantiate the requested specialization, inspect the resulting declaration and obtain its callable address. Compare taking a specialization's address with an explicit instantiation, then examine a class template as well. Follow how a type chosen as text becomes a concrete compiler entity before it can produce executable code or an object layout.

Taking `&scale<double>` requires a concrete specialization. Sema instantiates it while parsing, CodeGen emits it, and `Execute` makes the definition available to the JIT. Keeping the returned PTU lets the host follow the variable initializer to that specialization's `FunctionDecl`.

<!-- runnable-example: 4.5 -->

`examples/p05_templates.cpp`:

```cpp
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
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
template <typename T> T scale(T x) {
  return x * 3;
}
)cpp",
    R"cpp(
template <typename T> struct Box {
  T value;
  char tag;
  T other;
};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: address, explicit, class"),
      llvm::cl::cat(Options), llvm::cl::init("address"));
  llvm::cl::list<std::string> TypesOption(
      "type", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
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

  std::vector<std::string> Types(TypesOption.begin(), TypesOption.end());
  if (Types.empty()) {
    Types = {"int", "double"};
  }
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
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();

  // The canonical type a type name denotes ("unsigned" and "unsigned int" are
  // the same type but not the same spelling): declare an alias and read it
  // back.
  unsigned Counter = 0;

  for (size_t K = 0; K < Types.size(); ++K) {
    const std::string &Ty = Types[K];
    llvm::outs() << "type " << Ty << ":\n";
    QualType Want;

    std::string Alias = "selected_type_" + std::to_string(Counter++);
    llvm::Error InputError2 = I->ParseAndExecute(
        R"cpp(using )cpp" + Alias + R"cpp( = )cpp" + (Ty) + R"cpp(;)cpp");
    bool InputAccepted1 = !InputError2;
    if (InputError2) {
      llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                  "error: ");
    }
    if (!InputAccepted1) {
      continue;
    }
    clang::NamedDecl *FoundDeclaration;
    clang::ASTContext &LookupContext = S.getASTContext();
    clang::LookupResult Lookup(
        (S), clang::DeclarationName(&LookupContext.Idents.get((Alias))),
        clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
    S.LookupName(Lookup, S.TUScope);
    FoundDeclaration =
        Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
    auto *TA = llvm::dyn_cast_if_present<TypedefNameDecl>(FoundDeclaration);
    Want = TA ? TA->getUnderlyingType().getCanonicalType() : QualType();

    if (Want.isNull()) {
      continue;
    }
    if (Mode == "address") {
      // Take the address of the specialization: that forces its instantiation,
      // and the initializer of the new variable leads back to the FunctionDecl.
      // Build the input's AST and IR without running its initializers.
      auto ParsedInputOrError =
          I->Parse(R"cpp(auto function_pointer_)cpp" + std::to_string(K) +
                   R"cpp( = &scale<)cpp" + Ty + R"cpp(>;)cpp");
      if (!ParsedInputOrError) {
        llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(),
                                    llvm::errs(), "error: ");
        return 1;
      }
      PartialTranslationUnit &PTU = (*ParsedInputOrError);
      // The variable initializer refers to the instantiated function; follow it
      // to FunctionDecl.
      auto *VD = llvm::cast<VarDecl>(*PTU.TUPart->decls_begin());
      auto *UO = llvm::cast<UnaryOperator>(VD->getInit());
      auto *FD = llvm::cast<FunctionDecl>(
          llvm::cast<DeclRefExpr>(UO->getSubExpr())->getDecl());
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      // Resolve a runtime address; lookup may compile/link a module on demand.
      // The session owns code/storage; function-pointer types must match the
      // ABI.
      auto AddressOrError = I->getSymbolAddress(GlobalDecl(FD));
      if (!AddressOrError) {
        llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *Addr = (*AddressOrError).toPtr<void *>();
      llvm::outs() << "  specialization "
                   << FD->getType().getAsString(C.getPrintingPolicy()) << " at "
                   << (llvm::formatv("{0:x}", (uintptr_t)Addr).str()) << "\n";
      std::string NativeCallResult;

      const auto *BT = FD->getParamDecl(0)
                           ->getType()
                           .getCanonicalType()
                           ->getAs<BuiltinType>();
      llvm::raw_string_ostream NativeCallOutput(NativeCallResult);
      // The AST identifies a runtime-selected type, but native calls still need
      // a host C++ signature.
      switch (BT ? BT->getKind() : BuiltinType::Void) {
      case BuiltinType::Int: {
        auto Function = reinterpret_cast<int (*)(int)>((Addr));
        int Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<int>(Accumulated +
                                         Function(static_cast<int>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::UInt: {
        auto Function = reinterpret_cast<unsigned (*)(unsigned)>((Addr));
        unsigned Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<unsigned>(
              Accumulated + Function(static_cast<unsigned>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Long: {
        auto Function = reinterpret_cast<long (*)(long)>((Addr));
        long Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<long>(
              Accumulated + Function(static_cast<long>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Short: {
        auto Function = reinterpret_cast<short (*)(short)>((Addr));
        short Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<short>(
              Accumulated + Function(static_cast<short>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Float: {
        auto Function = reinterpret_cast<float (*)(float)>((Addr));
        float Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<float>(
              Accumulated + Function(static_cast<float>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      case BuiltinType::Double: {
        auto Function = reinterpret_cast<double (*)(double)>((Addr));
        double Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<double>(
              Accumulated + Function(static_cast<double>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      default:
        NativeCallOutput << "(host has no case for this type)";
      }

      llvm::outs() << "  host called it 1000000 times, sum = "
                   << NativeCallResult << "\n";
    } else if (Mode == "explicit") {
      // Ask for an explicit instantiation, then find the specialization in the
      // template.
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(template )cpp" + Ty + R"cpp( scale<)cpp" + Ty +
              R"cpp(>()cpp" + Ty + R"cpp();)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      clang::NamedDecl *FoundDeclaration2;
      clang::ASTContext &LookupContext = S.getASTContext();
      clang::LookupResult Lookup(
          (S), clang::DeclarationName(&LookupContext.Idents.get(("scale"))),
          clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
      S.LookupName(Lookup, S.TUScope);
      FoundDeclaration2 =
          Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
      auto *FT =
          llvm::dyn_cast_if_present<FunctionTemplateDecl>(FoundDeclaration2);
      if (!FT) {
        llvm::outs() << "  no function template 'scale' in the session\n";
        continue;
      }
      const FunctionDecl *Found = nullptr;
      // Find the concrete specialization by canonical type, not by printed
      // spelling.
      for (const FunctionDecl *Spec : FT->specializations()) {
        if (Spec->getTemplateSpecializationArgs()
                ->get(0)
                .getAsType()
                .getCanonicalType() == Want) {
          Found = Spec;
        }
      }
      if (!Found) {
        llvm::outs() << "  no specialization found\n";
        continue;
      }
      auto AddressOrError2 = I->getSymbolAddress(GlobalDecl(Found));
      if (!AddressOrError2) {
        llvm::logAllUnhandledErrors(AddressOrError2.takeError(), llvm::errs(),
                                    "error: ");
        return 1;
      }
      void *Addr = (*AddressOrError2).toPtr<void *>();
      llvm::outs() << "  specializations so far: "
                   << std::distance(FT->specializations().begin(),
                                    FT->specializations().end())
                   << ", this one "
                   << Found->getType().getAsString(C.getPrintingPolicy())
                   << "\n";
      std::string NativeCallResult2;

      const auto *BT = (Found)
                           ->getParamDecl(0)
                           ->getType()
                           .getCanonicalType()
                           ->getAs<BuiltinType>();
      llvm::raw_string_ostream NativeCallOutput(NativeCallResult2);
      switch (BT ? BT->getKind() : BuiltinType::Void) {
      case BuiltinType::Int: {
        auto Function = reinterpret_cast<int (*)(int)>((Addr));
        int Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<int>(Accumulated +
                                         Function(static_cast<int>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::UInt: {
        auto Function = reinterpret_cast<unsigned (*)(unsigned)>((Addr));
        unsigned Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<unsigned>(
              Accumulated + Function(static_cast<unsigned>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Long: {
        auto Function = reinterpret_cast<long (*)(long)>((Addr));
        long Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<long>(
              Accumulated + Function(static_cast<long>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Short: {
        auto Function = reinterpret_cast<short (*)(short)>((Addr));
        short Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<short>(
              Accumulated + Function(static_cast<short>(Index % 7)));
        }
        NativeCallOutput << Accumulated;
        break;
      }
      case BuiltinType::Float: {
        auto Function = reinterpret_cast<float (*)(float)>((Addr));
        float Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<float>(
              Accumulated + Function(static_cast<float>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      case BuiltinType::Double: {
        auto Function = reinterpret_cast<double (*)(double)>((Addr));
        double Accumulated = 0;
        for (int Index = 0; Index < 1000000; ++Index) {
          Accumulated = static_cast<double>(
              Accumulated + Function(static_cast<double>(Index % 7)));
        }
        NativeCallOutput << llvm::format("%.0f",
                                         static_cast<double>(Accumulated));
        break;
      }
      default:
        NativeCallOutput << "(host has no case for this type)";
      }

      llvm::outs() << "  host called it 1000000 times, sum = "
                   << NativeCallResult2 << "\n";
    } else if (Mode == "class") {
      // sizeof/alignof of the instantiation, asked of the interpreter as
      // Values...
      Value Size, Align;
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(sizeof(Box<)cpp" + Ty + R"cpp(>))cpp", &Size)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(alignof(Box<)cpp" + Ty + R"cpp(>))cpp", &Align)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
      llvm::outs() << "  Value:  sizeof = " << Size.convertTo<size_t>()
                   << ", alignof = " << Align.convertTo<size_t>() << "\n";
      // ...and read back from the AST: the sizeof expression instantiated
      // Box<T>.
      clang::NamedDecl *FoundDeclaration3;
      clang::ASTContext &LookupContext = S.getASTContext();
      clang::LookupResult Lookup(
          (S), clang::DeclarationName(&LookupContext.Idents.get(("Box"))),
          clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
      S.LookupName(Lookup, S.TUScope);
      FoundDeclaration3 =
          Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
      auto *CT =
          llvm::dyn_cast_if_present<ClassTemplateDecl>(FoundDeclaration3);
      if (!CT) {
        llvm::outs() << "  no class template 'Box' in the session\n";
        continue;
      }
      for (ClassTemplateSpecializationDecl *Spec : CT->specializations()) {
        if (Spec->getTemplateArgs()[0].getAsType().getCanonicalType() != Want) {
          continue;
        }
        // sizeof/alignof forced class instantiation; the resulting record now
        // has a layout.
        const ASTRecordLayout &L = C.getASTRecordLayout(Spec);
        llvm::outs() << "  layout: size " << L.getSize().getQuantity()
                     << ", align " << L.getAlignment().getQuantity()
                     << ", fields at";
        unsigned N = 0;
        for (const FieldDecl *F : Spec->fields()) {
          llvm::outs() << " " << F->getName() << "@"
                       << L.getFieldOffset(N++) / 8;
        }
        llvm::outs() << "\n";
      }
    }
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_templates.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p05_templates
out/p05_templates --mode=address --type=int --type=double -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_templates.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p05_templates
out/p05_templates --mode=address --type=int --type=double -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

Explicit instantiation reaches the same result through a declaration such as `template double scale<double>(double);`. After that input, `FunctionTemplateDecl::specializations()` lists the instantiated functions, and their template arguments identify the requested type.

Compare the reference tool's two routes:

```bash
build/bin/p05_templates manifests/p05_templates.repl address int double float long
```

```text expected
type int:
  specialization int (int) at 0xADDR
  host called it 1000000 times, sum = 8999991
type double:
  specialization double (double) at 0xADDR
  host called it 1000000 times, sum = 8999991
type float:
  specialization float (float) at 0xADDR
  host called it 1000000 times, sum = 8999991
type long:
  specialization long (long) at 0xADDR
  host called it 1000000 times, sum = 8999991
```

```bash
build/bin/p05_templates manifests/p05_templates.repl explicit int double float long
```

```text expected
type int:
  specializations so far: 1, this one int (int)
  host called it 1000000 times, sum = 8999991
type double:
  specializations so far: 2, this one double (double)
  host called it 1000000 times, sum = 8999991
type float:
  specializations so far: 3, this one float (float)
  host called it 1000000 times, sum = 8999991
type long:
  specializations so far: 4, this one long (long)
  host called it 1000000 times, sum = 8999991
```

The explicit route's specialization count grows as types are requested. Both routes use `getSymbolAddress(GlobalDecl(FD))`, so the chosen specialization supplies the name for lookup.

The host still needs a compiled C++ calling type. This example switches on supported canonical builtin types and calls through a matching function pointer in each branch. For other types, generate a wrapper with a fixed host interface rather than guessing how an arbitrary runtime type is passed.

Class templates can also be instantiated by expressions that require their layout:

```bash
build/bin/p05_templates manifests/p05_templates.repl class int char double
```

```text expected
type int:
  Value:  sizeof = 12, alignof = 4
  layout: size 12, align 4, fields at value@0 tag@4 other@8
type char:
  Value:  sizeof = 3, alignof = 1
  layout: size 3, align 1, fields at value@0 tag@1 other@2
type double:
  Value:  sizeof = 24, alignof = 8
  layout: size 24, align 8, fields at value@0 tag@8 other@16
```

`sizeof(Box<T>)` and `alignof(Box<T>)` return scalar Values, and the resulting specialization's AST layout confirms those numbers. Size alone does not create an object: storage also needs correct alignment and generated construction before use.

Predict the layout for `short` and `unsigned long`:

```bash
build/bin/p05_templates manifests/p05_templates.repl class short "unsigned long"
```

```text expected
type short:
  Value:  sizeof = 6, alignof = 2
  layout: size 6, align 2, fields at value@0 tag@2 other@4
type unsigned long:
  Value:  sizeof = 24, alignof = 8
  layout: size 24, align 8, fields at value@0 tag@8 other@16
```

The different padding follows their element sizes and alignment. Comparing canonical types also lets the tool recognize equivalent type spellings.

> [!hint]- Why keep a PTU in the address-taking route?
> The initializer contains the specialization the host wants to inspect.

> [!success]- Follow instantiation to execution
> `Parse` exposes that AST before `Execute` submits its code. The explicit route instead finds the specialization through the template after `ParseAndExecute` completes.

Change the multiplier from three to four in a copied template fixture and request `int` and `double`. Match each specialization's signature to the host pointer type used for its call.

---

## Section 4.6 — Choose an observation point

The same source can be viewed at several points: as a diagnostic, a declaration, a type, a generated module or a runtime symbol. Choosing the right point keeps a host's design clear. A question about overloads belongs with compiler declarations; a request to call the selected function eventually needs executable code.

Follow one function through those views and identify which API supplies each answer. Introduce an error, repair it and continue towards a callable address. This brings the compiler services together and shows which handles depend on the continued lifetime of the session.

Take one function from a copied fixture and introduce a type error in its body. Collect the diagnostic, repair the function and locate its declaration. Inspect its type before executing the PTU, then resolve its address and call it.

Mark the point at which each observation becomes available. Diagnostics, lookup and types use compiler state; a callable address needs a runtime definition. That distinction lets a host reject an unsuitable declaration before executing its module.

Now remove or replace the input and repeat the lookup. Any retained declaration or address must be tied to the session that owns it. Reacquiring it after a change keeps the host from confusing an old handle with the current definition.

---

[← Part 3 — Connecting Host and JIT](part_3_under_the_hood.md) | [Part 5 — Values, Objects and Lifetimes →](part_5_values_and_data.md)

