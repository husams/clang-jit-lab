# Part 3 — Connecting Host and JIT

[← Part 2 — Incremental Compilation](part_2_power_user_shell.md) | [Part 4 — Clang Compiler Services →](part_4_embedding_api.md)

Make generated code useful to its host: call across the boundary, exchange data, load definitions and decide when addresses become invalid.

## Section 3.1 — Calling JIT code from the host

A host that repeatedly applies a compiled rule should not have to submit a new expression string for every call. Once the function exists, the application can obtain its address and use an ordinary native function pointer to pass arguments and receive results.

Explore how symbol lookup reaches functions and global variables, including C++ names that are mangled. Compare lookup by an IR name, a declaration and a linker name, then keep one resolved pointer outside a host loop. The important connection is between Clang's declaration, the linker's symbol and the ABI type used at the call.

Symbol lookup begins with the information the host has: a source declaration, an IR name or a name from an object file.

| Call | Takes | When |
|------|-------|------|
| `getSymbolAddress(StringRef IRName)` | the name as written in the IR: `c_add`, `_Z7cxx_mulii` | you know the name |
| `getSymbolAddress(GlobalDecl)` | a `clang::GlobalDecl` made from a `FunctionDecl*` / `VarDecl*` | you have the AST node: no mangling by hand |
| `getSymbolAddressFromLinkerName(StringRef)` | the target’s linker spelling: Mach-O normally adds an underscore; ELF normally does not | you got the name from a linker-level tool |

Each route returns `llvm::Expected<llvm::orc::ExecutorAddr>`. On success, `toPtr<Fn>()` gives the address the host's calling type; that type must match the definition.

<!-- runnable-example: 3.1 -->

`examples/p04_call.cpp`:

```cpp
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/Mangle.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DataLayout.h>
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

  // --- 1. by IR name: extern "C" has no mangling, C++ does ---
  llvm::outs()
      << "== getSymbolAddress(StringRef): the name as written in the IR\n";
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = I->ParseAndExecute(
          R"cpp(
extern "C" int c_add(int a, int b) {
  return a + b;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int cxx_mul(int a, int b) {
  return a * b;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  auto AddressOrError = I->getSymbolAddress("c_add");
  if (!AddressOrError) {
    llvm::logAllUnhandledErrors(AddressOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto CAdd = std::move(*AddressOrError);
  auto AddressOrError2 = I->getSymbolAddress("_Z7cxx_mulii");
  if (!AddressOrError2) {
    llvm::logAllUnhandledErrors(AddressOrError2.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto CxxMul = std::move(*AddressOrError2);
  llvm::outs() << "c_add(2, 3)          = "
               << CAdd.toPtr<int (*)(int, int)>()(2, 3) << "\n";
  llvm::outs() << "_Z7cxx_mulii(4, 5)   = "
               << CxxMul.toPtr<int (*)(int, int)>()(4, 5) << "\n";
  llvm::outs() << "lookup of \"cxx_mul\"  : "
               << llvm::toString(I->getSymbolAddress("cxx_mul").takeError())
               << "\n";

  // --- 2. by GlobalDecl: no manual mangling ---
  llvm::outs()
      << "== getSymbolAddress(GlobalDecl): take the decl from the PTU\n";
  // Build the input's AST and IR without running its initializers.
  auto ParsedInputOrError = I->Parse(R"cpp(
double scale(double v) {
  return v * 1.5;
}
)cpp");
  if (!ParsedInputOrError) {
    llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  PartialTranslationUnit &PTU = (*ParsedInputOrError);
  FunctionDecl *FD = nullptr;
  // Find the actual FunctionDecl, then let GlobalDecl supply the mangled lookup
  // name.
  for (Decl *D : PTU.TUPart->decls()) {
    if (auto *F = llvm::dyn_cast<FunctionDecl>(D)) {
      FD = F;
    }
  }
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error Error = I->Execute(PTU)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  auto AddressOrError3 = I->getSymbolAddress(GlobalDecl(FD));
  if (!AddressOrError3) {
    llvm::logAllUnhandledErrors(AddressOrError3.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto Scale = std::move(*AddressOrError3);
  llvm::outs() << FD->getQualifiedNameAsString()
               << "(2.0) = " << Scale.toPtr<double (*)(double)>()(2.0) << "\n";

  // The same name, computed by hand with a MangleContext.
  // MangleContext derives ABI names from declarations; avoid encoding C++ names
  // by hand.
  std::unique_ptr<MangleContext> MC(I->getASTContext().createMangleContext());
  std::string Mangled;
  llvm::raw_string_ostream OS(Mangled);
  MC->mangleName(GlobalDecl(FD), OS);
  llvm::outs() << "MangleContext says: " << Mangled << "\n";

  // --- 3. by linker name: Mach-O adds a leading underscore ---
  llvm::outs() << "== getSymbolAddressFromLinkerName\n";
  std::string LinkerName;
  // LLVM's DataLayout knows whether the target object format adds a prefix.
  // Derive the linker's global prefix from the target instead of assuming
  // Mach-O or ELF.
  llvm::DataLayout Layout(
      I->getASTContext().getTargetInfo().getDataLayoutString());
  if (char Prefix = Layout.getGlobalPrefix()) {
    LinkerName += Prefix;
  }
  LinkerName += "_Z7cxx_mulii";
  auto AddressOrError4 = I->getSymbolAddressFromLinkerName(LinkerName);
  if (!AddressOrError4) {
    llvm::logAllUnhandledErrors(AddressOrError4.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  auto ByLinker = std::move(*AddressOrError4);
  llvm::outs() << "same address as the IR name: "
               << (ByLinker == CxxMul ? "yes" : "no") << "\n";
  llvm::outs() << "IR name as a linker name   : ";
  if (auto IRAsLinker = I->getSymbolAddressFromLinkerName("_Z7cxx_mulii")) {
    llvm::outs() << "found (no extra target prefix)\n";
  } else {
    llvm::outs() << llvm::toString(IRAsLinker.takeError()) << "\n";
  }

  // --- 4. data: read and write a JIT global ---
  llvm::outs() << "== a JIT global through its address\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int counter = 10;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  auto AddressOrError5 = I->getSymbolAddress("counter");
  if (!AddressOrError5) {
    llvm::logAllUnhandledErrors(AddressOrError5.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  int *Counter = (*AddressOrError5).toPtr<int *>();
  llvm::outs() << "host reads counter = " << *Counter << "\n";
  *Counter = 99;
  llvm::outs() << "host wrote 99, the JIT sees:\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
counter
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
counter += 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  llvm::outs() << "the JIT added 1, the host sees counter = " << *Counter
               << "\n";

  // --- 5. call it many times: a plain function pointer from now on ---
  llvm::outs() << "== a host loop calling JIT code\n";
  auto Add = CAdd.toPtr<int (*)(int, int)>();
  long Sum = 0;
  for (int K = 1; K <= 100000; ++K) {
    Sum += Add(K, 1);
  }
  llvm::outs() << "100000 calls, final value " << Sum << "\n";
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_call.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_call
out/p04_call -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_call.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_call
out/p04_call -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The reference run compares those lookup routes and shares a global variable:

```bash
build/bin/p04_call
```

```text expected
== getSymbolAddress(StringRef): the name as written in the IR
c_add(2, 3)          = 5
_Z7cxx_mulii(4, 5)   = 20
lookup of "cxx_mul"  : Symbols not found: [ _cxx_mul ]
== getSymbolAddress(GlobalDecl): take the decl from the PTU
scale(2.0) = 3.000000e+00
MangleContext says: _Z5scaled
== getSymbolAddressFromLinkerName
same address as the IR name: yes
IR name as a linker name   : Symbols not found: [ _Z7cxx_mulii ]
== a JIT global through its address
host reads counter = 10
host wrote 99, the JIT sees:
(int) 99
the JIT added 1, the host sees counter = 100
== a host loop calling JIT code
100000 calls, final value 5000150000
```

`c_add` has C linkage, so its IR name is simply `c_add`. The C++ function `cxx_mul(int, int)` uses `_Z7cxx_mulii` under the tested Itanium ABI. A `GlobalDecl` avoids spelling that mangled name: Clang derives it from the actual declaration. `MangleContext::mangleName` is useful when you need to display the derived name yourself.

Linker spelling can add a target prefix. The reference tool derives it from `DataLayout`; on Mach-O the extra underscore makes the C++ linker name `__Z7cxx_mulii`, while ELF normally keeps the IR spelling.

A global lookup returns storage rather than code. Converting `counter`'s address to `int *` lets the host read and update the same variable the JIT uses. The repeated-call loop similarly reuses one resolved function address; it does not involve the compiler on each iteration.

Inspect an overload pair and predict why a plain source name cannot select one address:

```bash
mkdir -p out
cat > out/p04_overloads.repl <<'EOF'
int pick(int v) { return 1; }
int pick(double v) { return 2; }
EOF
build/bin/p04_ir out/p04_overloads.repl --full | grep -E '^define' | sed 's/(.*//'
```

```text expected
define noundef i32 @_Z4picki
define noundef i32 @_Z4pickd
```

The `int` and `double` parameters produce different mangled names. A chosen `FunctionDecl` identifies the particular overload, but the host must still check its type before calling it.

> [!hint]- What if the pointer says `int(int)` and the definition says `void(double)`?
> `toPtr` converts the address without inspecting the AST.

> [!success]- Follow the calling convention
> No automatic diagnostic protects that call; it has undefined behavior. Compare the declaration's type with the intended interface before converting the address.

Change `scale` in a copy of the standalone program to accept and return `float`, and update the host pointer to `float (*)(float)`. Call it with `2.0f` using declaration-based lookup. Keep the interpreter alive for the call, and discard the address when its defining input is undone.

---

## Section 3.2 — Sharing state with the host

Suppose generated code should update an application counter or send a message through the host's logging function. Both definitions already exist in the executable; the JIT needs a way to refer to them rather than creating another copy.

Give the interpreter matching declarations and let symbol resolution connect them to the host's exported definitions. Watch a counter change from both sides, then call a host function from generated code. This shows how declarations provide the compiler with types while runtime symbols provide the linker with the actual storage or callable address.

The JIT needs both a declaration for C++ checking and a definition for linking. This example declares the host's exported functions and variables in the session, then lets ORC resolve them against the running process.

<!-- runnable-example: 3.2 -->

`examples/p05_shared.cpp`:

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
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_shared.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p05_shared
out/p05_shared --mode=extern -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_shared.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p05_shared
out/p05_shared --mode=extern -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

On Linux the host link command exports executable symbols with `-Wl,--export-dynamic`. The following macOS symbol listing shows the corresponding Mach-O spellings:

```bash
nm -gU build/bin/p05_shared | awk '{print $2, $3}' | grep -i host_ | sort
```

```text expected
D _host_counter
D _host_name
T __Z12host_cpp_addii
T _host_log
T _host_twice
```

Watch the host and generated code take turns updating `host_counter`:

```bash
build/bin/p05_shared extern
```

```text expected
host sees host_counter = 10
(int) 10
host sees host_counter = 15 after the interpreter added 5
(int) 100
(int) 42
(const char *) "the host"
  [host_log] called from interpreted code
&host_counter in the JIT == &host_counter in C++: 1
```

The address comparison explains the result: both sides refer to one object. Normal C++ lifetime and concurrency rules apply, including synchronization for concurrent writes.

C++ linkage works when the embedded declaration produces the same mangled symbol as the host definition:

```bash
build/bin/p05_shared cpp
```

```text expected
(int) 5
(int) 400
host calls jit_sub(9, 4) = 5
```

An internal function gives the linker a different problem. Calling it normally inside the host works, but declaring the same source name in the interpreter does not make the host's private definition available:

```bash
build/bin/p05_shared static 2>&1 | sed -E 's/(Failed to materialize symbols:).*/\1 .../'
```

```text expected
host_hidden(1) in C++ = 2
JIT session error: Symbols not found: [ __Z11host_hiddeni ]
error: Failed to materialize symbols: ...
```

The call type-checks and then fails during symbol resolution. Hidden visibility and linker removal can likewise prevent a process lookup from finding a definition.

The host can also reach generated definitions. Here it calls `jit_add` repeatedly and reads the JIT-owned `jit_total`:

```bash
build/bin/p05_shared jit2host
```

```text expected
host called jit_add 3 times; jit_total read from C++ = 6
(int) 1000
```

Compare the C and C++ names exported by the reference executable:

```bash
nm -gU build/bin/p05_shared | awk '{print $3}' | grep -E 'host_(twice|cpp_add)|ns_scale' | sort
```

```text expected
__Z12host_cpp_addii
__ZN4host8ns_scaleEi
_host_twice
```

`extern "C"` simplifies the spelling; C++ declarations must also agree on namespace and parameter types. Section 3.5 will publish a private function explicitly when export lookup is unsuitable.

> [!hint]- What changes when the callbacks live in a loaded library?
> Consider which loaded image exports their definitions.

> [!success]- Follow process lookup
> The library must expose the symbols to dynamic lookup. Resolution uses the loaded process's symbols, so names can also collide with another loaded library.

Change the standalone program's JIT-side increment from five to eight. Predict both views of the counter, rebuild and run its `--mode=extern` invocation. Then use its `--mode=static` path to locate the boundary between compiler acceptance and linker visibility.

---

## Section 3.3 — Injecting host data into JIT code

A host may want to lend generated code an existing configuration object or buffer that has no exported symbol name. Its address can provide the connection: a pointer created in the JIT can refer to that same object in the shared process address space.

Supply a matching type declaration and build the pointer from the host object's address. Change a field through JIT code and read it again in the host to see that no separate object was created. Follow the type layout and lifetime as carefully as the address, because the JIT is accessing the host's actual memory.

The pointer declaration created in the session contains the host object's address. Reading through it reaches the original storage. The two `Config` definitions must agree on fields, packing, target and ABI, and the host object must remain alive through every generated access.

<!-- runnable-example: 3.3 -->

`examples/p05_inject.cpp`:

```cpp
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
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_inject.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p05_inject
out/p05_inject --mode=config -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p05_inject.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p05_inject
out/p05_inject --mode=config -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

`llvm::raw_string_ostream` inserts the runtime address into the generated source. `ParseAndExecute` compiles that declaration, after which later inputs can use the pointer as ordinary C++.

```bash
build/bin/p05_inject config
```

```text expected
> auto *cfg = (Config *)0xADDRUL;
> cfg->verbosity
(int) 1
> cfg->name
(char[16]) "host"
> cfg->limits[2]
(int) 30
> cfg->scale *= 2; cfg->verbosity = 7; cfg->limits[3] = -1;
> __builtin_strcpy(cfg->name, "jit wrote");
host now sees: verbosity=7 scale=3 name=jit wrote limits[3]=-1
> sizeof(Config)
(unsigned long) 48
host sizeof(Config) = 48
```

The reference `config` run reads the host's fields and writes back to the same object. Its reported size, 48 bytes on the recorded target, comes from the matching layout on both sides.

A callback uses the same idea with a function-pointer type:

```bash
build/bin/p05_inject callback
```

```text expected
> auto report = (ReportFn)0xADDRUL;
> report("answer", 42);
  [host_report] answer = 42
> for (int i = 1; i <= 3; ++i) report("square", i * i);
  [host_report] square = 1
  [host_report] square = 4
  [host_report] square = 9
```

The function's address and its declared signature must both be correct. For an array, the host also supplies the length so generated code knows which elements it may access:

```bash
build/bin/p05_inject array
```

```text expected
> int *data = (int *)0xADDRUL; const int n = 8;
> int sum = 0; for (int i = 0; i < n; ++i) sum += data[i];
> sum
(int) 31
host computes the same sum: 31
> for (int i = 0; i < n; ++i) data[i] *= data[i];
host array after the interpreter squared it: 9 1 16 1 25 81 4 36
```

The reverse direction starts with a captured class lvalue. Its Value borrows the JIT object's address, which lets the host access the same storage when it knows the layout:

```bash
build/bin/p05_inject reverse
```

```text expected
> Config jit_cfg = {3, 0.25, "made in jit", {1, 2, 3, 4}};
> jit_cfg
host reads jit_cfg: verbosity=3 scale=0.25 name=made in jit limits[3]=4
> jit_cfg.verbosity
(int) 99
> Config{5, 0.5, "temporary", {}}
host reads the temporary: name=temporary owned=1 same address as jit_cfg: 0
```

The host's write to `verbosity` appears in the next JIT expression. A temporary differs: its Value owns separate storage, as Part 5 explains. The interpreter's own capture machinery also embeds addresses in generated expressions to connect running code to host result storage.

Predict the padding between `verbosity` and `scale`, then inspect the full layout:

```bash
mkdir -p out
printf '%s\n' 'struct Config { int verbosity; double scale; char name[16]; int limits[4]; };' > out/p05_cfg.repl
build/bin/p05_layout out/p05_cfg.repl layout Config
```

```text expected
struct Config: size 48, align 8, data size 48, non-virtual size 48
  offset  bits  what
       0    32  verbosity : int
       4    32  <padding>
       8    64  scale : double
      16   128  name : char[16]
      32   128  limits : int[4]
```

The `double` needs alignment, leaving four bytes after the first `int` on this target. Obtain offsets from the current AST rather than relying on this one transcript.

> [!hint]- What if a macro changes `Config` in only one compiler?
> The pointer can be valid while field offsets disagree.

> [!success]- Follow the field access
> Each compiler uses its own layout, so both sides may compile successfully and still read different bytes. Compare sizes and offsets or share the same declaration and compatible options.

Change one input array element in the standalone example, calculate the sum and rerun `--mode=array`. Identify which object owns the buffer and when the borrowed address stops being usable. These addresses work only within this process.

---

## Section 3.4 — Load a supporting library through the API

You may want a JIT-compiled function to call code from a supporting library. There are two pieces to that connection: Clang needs a declaration to check the call, and the runtime linker needs a compiled definition to supply its target.

Build a small shared library, load it through `LoadDynamicLibrary`, and submit a declaration for its function. Then evaluate a call and trace how its name reaches the library's implementation. Separating the declaration from the loaded definition will help you recognize a compilation error versus a missing-symbol failure.

The library defines `lab_triple`; the host supplies the declaration that lets Clang compile a call to it. Build and load the reference library on macOS:

```bash
mkdir -p out
"$LLVM/bin/clang++" -dynamiclib manifests/p03_library.cpp \
  -o out/p03_library.dylib
scripts/build.sh p03_library
```

```bash
build/bin/p03_library out/p03_library.dylib
```

```text expected
library result = 42
```

Linux uses an ELF shared object:

```bash
"$LLVM/bin/clang++" -fPIC -shared manifests/p03_library.cpp \
  -o out/p03_library.so
build/bin/p03_library out/p03_library.so
```

Follow the standalone host's load call and the later source submission:

<!-- runnable-example: 3.4 -->

`examples/support_library.cpp`:

```cpp
// Export a C-linkage definition; the JIT still needs a matching declaration to compile a call.
extern "C" int triple(int x) { return 3 * x; }
```

`examples/p03_library.cpp`:

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

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Library(
      "library", llvm::cl::desc("Shared library to load"),
      llvm::cl::cat(Options), llvm::cl::Required);
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
  auto Engine = std::move(*InterpreterOrError);
  // Expose the library's exported definitions to linking; C++ declarations are
  // still needed.
  if (llvm::Error Error = Engine->LoadDynamicLibrary(Library.c_str())) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = Engine->ParseAndExecute(R"cpp(
extern "C" int triple(int);
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Capture the call's result after the declaration and loaded definition are
  // both available.
  clang::Value Result;
  if (llvm::Error Error = Engine->ParseAndExecute(R"cpp(
triple(14)
)cpp",
                                                  &Result)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  llvm::outs() << "library result = " << Result.convertTo<int>() << "\n";
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p03_library.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p03_library
"$LLVM/bin/clang++" -fPIC -shared examples/support_library.cpp -o out/p03_library.so
out/p03_library --library=out/p03_library.so -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p03_library.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p03_library
"$LLVM/bin/clang++" -dynamiclib examples/support_library.cpp -o out/p03_library.dylib
out/p03_library --library=out/p03_library.dylib -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

`LoadDynamicLibrary` makes the library's exported definitions available to runtime lookup. It does not parse C++ declarations from the binary, which is why the JIT source still declares the function before calling it. Handle a load failure before submitting dependent code.

Change the multiplier to four, rebuild the library and launch a fresh host. Predict the result. Then supply a nonexistent library path and follow the load-error branch. Rebuilding a file does not replace code already loaded in an existing process.

---

## Section 3.5 — Register a host address explicitly

Sometimes the host has a useful callback that ordinary process-symbol lookup cannot find, such as a function with internal linkage. The host still knows its address and can deliberately publish a callable name for generated code to use.

Add that name and address to the JIT's symbol namespace with `absoluteSymbols`. Pair the registration with a matching C++ declaration, then call the function from the JIT. Notice the separate roles of the declaration, the registered symbol spelling and the function's real address.

A known function address can be published under a chosen runtime symbol, even when the function has internal linkage. The reference tool's `--abs` path makes that difference visible:

```bash
build/bin/p06_jit --abs manifests/p06_hostcall.repl
```

```text expected
LLJIT reached through the callback: yes
input: #include <stdio.h>
input: extern "C" int host_exported();
input: extern "C" int host_secret();
input: printf("exported: %d\n", host_exported());
exported: 7
input: printf("secret: %d\n", host_secret());
secret: 42
registered with debuggers (__jit_debug_descriptor): no
```

The standalone program installs the definition while constructing `LLJIT`:

<!-- runnable-example: 3.5 -->

`examples/p06_jit.cpp`:

```cpp
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
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p06_jit.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p06_jit
out/p06_jit --abs -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p06_jit.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p06_jit
out/p06_jit --abs -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The creation callback receives the live JIT. `mangleAndIntern` supplies the target's symbol spelling, and `absoluteSymbols` associates it with an address and symbol flags in the main `JITDylib`. The C++ declaration in the session then gives Clang the type needed for a call.

Compare the reference run without `--abs`: the declaration still compiles, but the private definition cannot be found by process lookup. Add a second static callback to a copy of the standalone program, register its address under another name and submit a call to it.

Keep that address tied to its owner's lifetime. It is meaningful in the current process; a separate executor cannot call an arbitrary host pointer.

---

## Section 3.6 — `Undo` from the API

An incremental session remembers earlier inputs. If a user withdraws the last definition or your application backtracks after an experiment, both the compiler's view of that definition and its generated code need to change together.

Use `Undo` to remove recent successful inputs, then inspect the name through AST lookup and JIT symbol lookup. Try defining the name again after removal. Keep track of what disappears and which earlier inputs remain, while remembering that removing an input cannot reverse arbitrary effects its executed code already caused.

`Undo(N)` removes recent accepted inputs from both compiler lookup and the JIT. The unit it counts is an input, so a single input can remove several declarations together.

<!-- runnable-example: 3.6 -->

`examples/p04_undo.cpp`:

```cpp
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
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
  llvm::cl::list<std::string> InputsOption(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("C++ input; repeat for an incremental sequence"),
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

  if (!InputsOption.empty()) { // run the arguments as inputs, then
                               // Undo() until it fails
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
    // Extract the failed Expected with takeError(); logging consumes that
    // Error.
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
    std::unique_ptr<Interpreter> J = std::move(*InterpreterOrError);
    for (const std::string &Code : InputsOption) {
      // Compile this input, then submit its module and run its initializers.
      if (llvm::Error Error = (J)->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
    }
    unsigned Undone = 0;
    while (true) {
      // Remove recent accepted inputs and their symbols; earlier writes and I/O
      // remain.
      llvm::Error E = J->Undo();
      if (E) {
        llvm::consumeError(std::move(E));
        break;
      }
      ++Undone;
    }
    llvm::outs() << "Undo() succeeded " << Undone << " times\n";
    return 0;
  }

  std::vector<const char *> CompilerArguments2;
  for (const std::string &Flag : CompilerFlags) {
    CompilerArguments2.push_back(Flag.c_str());
  }
  clang::IncrementalCompilerBuilder CompilerBuilder2;
  CompilerBuilder2.SetCompilerArgs(CompilerArguments2);
  auto CompilerOrError2 = CompilerBuilder2.CreateCpp();
  if (!CompilerOrError2) {
    llvm::logAllUnhandledErrors(CompilerOrError2.takeError(), llvm::errs(),
                                "compiler: ");
    return 1;
  }

  auto InterpreterOrError2 =
      clang::Interpreter::create(std::move(*CompilerOrError2), nullptr);
  if (!InterpreterOrError2) {
    llvm::logAllUnhandledErrors(InterpreterOrError2.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError2);

  llvm::outs() << "== undo a definition\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int base = 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f() {
  return base + 1;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&IArgument = (*I);
    auto &&WhenArgument = ("before Undo");
    auto &&NameArgument = ("f");
    auto &&IRNameArgument = ("_Z1fv");

    bool IsDeclared;
    // AST lookup tests compiler visibility; JIT lookup separately tests the
    // runtime symbol.
    clang::ASTContext &Context = (IArgument).getASTContext();
    clang::DeclarationName Declaration(&Context.Idents.get((NameArgument)));
    IsDeclared = !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
    llvm::outs() << "  " << WhenArgument
                 << ": declared=" << (IsDeclared ? "yes" : "no")
                 << " JIT symbol ";
    // Resolve a runtime address; lookup may compile/link a module on demand.
    // The session owns code/storage; function-pointer types must match the ABI.
    if (auto Addr = IArgument.getSymbolAddress(IRNameArgument)) {
      llvm::outs() << "found\n";
    } else {
      llvm::outs() << "missing (" << llvm::toString(Addr.takeError()) << ")\n";
    }
  }
  if (llvm::Error Error = I->Undo()) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&IArgument2 = (*I);
    auto &&WhenArgument2 = ("after Undo ");
    auto &&NameArgument2 = ("f");
    auto &&IRNameArgument2 = ("_Z1fv");

    bool IsDeclared2;
    clang::ASTContext &Context = (IArgument2).getASTContext();
    clang::DeclarationName Declaration(&Context.Idents.get((NameArgument2)));
    IsDeclared2 =
        !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
    llvm::outs() << "  " << WhenArgument2
                 << ": declared=" << (IsDeclared2 ? "yes" : "no")
                 << " JIT symbol ";
    if (auto Addr = IArgument2.getSymbolAddress(IRNameArgument2)) {
      llvm::outs() << "found\n";
    } else {
      llvm::outs() << "missing (" << llvm::toString(Addr.takeError()) << ")\n";
    }
  }
  bool IsDeclared3;
  clang::ASTContext &Context = (*I).getASTContext();
  clang::DeclarationName Declaration(&Context.Idents.get(("base")));
  IsDeclared3 = !Context.getTranslationUnitDecl()->lookup(Declaration).empty();
  llvm::outs() << "  base is untouched: declared="
               << (IsDeclared3 ? "yes" : "no") << "\n";
  llvm::outs() << "  calling f():\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
f()
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== redefine after undo\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int f() {
  return base + 100;
}
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
f()
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== undo a global with a different type, then redefine it\n";
  if (llvm::Error Error = I->Undo(2)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  } // the new f() and the f() call that printed 101
  if (llvm::Error Error = I->Undo()) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  } // int base
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
double base = 2.5;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
base
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== the lower bound\n";
  // Inputs so far: int base (undone), double base, base  => 2 live inputs.
  llvm::outs() << "Undo(3): " << llvm::toString(I->Undo(3)) << "\n";
  llvm::outs() << "Undo(2): " << llvm::toString(I->Undo(2)) << "|\n";
  llvm::outs() << "Undo(1): " << llvm::toString(I->Undo(1)) << "\n";
  llvm::outs() << "the prelude is still there:\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
1 + 1
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
  examples/p04_undo.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_undo
out/p04_undo -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_undo.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_undo
out/p04_undo -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

Compare the AST and symbol lookups in the reference run:

```bash
build/bin/p04_undo
```

```text expected
== undo a definition
  before Undo: declared=yes JIT symbol found
  after Undo : declared=no JIT symbol missing (Symbols not found: [ __Z1fv ])
  base is untouched: declared=yes
  calling f():
In file included from <<< inputs >>>:1:
input_line_3:1:1: error: use of undeclared identifier 'f'
    1 | f()
      | ^
error: Parsing failed.
== redefine after undo
(int) 101
== undo a global with a different type, then redefine it
(double) 2.5000000
== the lower bound
Undo(3): Operation failed. Wanted to undo 3 inputs, only have 2.
Undo(2): |
Undo(1): Operation failed. No input left to undo
the prelude is still there:
(int) 2
```

Removing `f` leaves the earlier `base` input in place. A new definition of `f` is then accepted, and removing `base` allows a replacement with a different type. Both the old declaration and old runtime definition must disappear for that to work.

A rejected parse does not add an input to undo. A result-producing expression does, because it created generated code even without introducing a user name. Predict the count in this run:

```bash
build/bin/p04_undo 'int a = 1;' 'a' 'int a = 2;' 'a' 2>&1 | tail -n 1
```

```text expected
Undo() succeeded 3 times
```

Three inputs were accepted; the redefinition failed. The interpreter protects its runtime prelude, so undoing all user inputs does not remove the support needed by later expressions.

Removing an input does not reverse its effects. If an assignment already changed `base`, undo removes that assignment's code while leaving the written value. In LLVM 22.1.8, global destructor registration also needs care: removing an input is not a reliable object-retirement policy.

> [!hint]- What does undoing a bare expression remove?
> Follow its `TopLevelStmtDecl` and generated initializer.

> [!success]- Follow the generated input
> It removes the expression's PTU and module. No user declaration need disappear, and a result or external effect already produced is not undone.

Add an assignment to the example, undo that input and read the earlier variable again. Draw the surviving inputs after each operation. Drop declaration pointers and callable addresses associated with every removed input; never test an old address by calling it.

---

## Section 3.7 — Initializers and exception boundaries

Defining a function prepares code for a later call, but defining a global object can introduce work that runs as soon as its module is executed. Constructors, later function calls and session cleanup therefore occupy different points in the lifetime of generated code.

Use an object that prints when it is constructed and destroyed to follow that order. Then consider where an exception or other native failure would cross back into the host. Compiler acceptance tells you that the source passed language checks; it does not promise that running the accepted code will finish normally.

A global with a nontrivial constructor makes module initialization observable:

```bash
build/bin/p04_hello '#include <cstdio>' \
  'struct Trace { Trace() { printf("constructed\n"); } ~Trace() { printf("destroyed\n"); } };' \
  'Trace guard;' 'printf("after construction\n");'
```

```text expected
input: #include <cstdio>
input: struct Trace { Trace() { printf("constructed\n"); } ~Trace() { printf("destroyed\n"); } };
input: Trace guard;
constructed
input: printf("after construction\n");
after construction
destroyed
```

`Parse` produces the constructor call in IR. `Execute` runs the module's initialization and creates `guard`. The registered destructor later needs the object's code to remain available during session cleanup.

Add a second object as a later input and predict the destruction order. Compare normal interpreter destruction with input removal; `Undo` does not promise to retire every object and registered destructor safely.

A native exception belongs to the running C++ program. It does not automatically turn into `llvm::Error`. A generated wrapper can catch it and return an application result through a fixed interface:

```bash
build/bin/p04_hello '#include <stdexcept>' \
  'extern "C" int checked(int n) { try { if (n < 0) throw std::runtime_error("negative"); return n * 7; } catch (const std::exception &) { return -1; } }' \
  'checked(6)' 'checked(-1)'
```

```text expected
input: #include <stdexcept>
input: extern "C" int checked(int n) { try { if (n < 0) throw std::runtime_error("negative"); return n * 7; } catch (const std::exception &) { return -1; } }
input: checked(6)
(int) 42
input: checked(-1)
(int) -1
```

Change the wrapper to reject zero as well as negative numbers. Test both paths, then call it from a host through `int (*)(int)`. The integer reports the wrapper's result; compiler and linker failures still arrive through their separate API error paths. Propagating exceptions across a host/JIT ABI or a process boundary requires a supported runtime contract.

> [!hint]- Which operation constructs `guard`?
> Separate accepting its declaration from running its initializer.

> [!success]- Follow initialization
> `Execute` runs the generated initializer. A later function call is another execution path, and session cleanup runs the registered destructor.

---

[← Part 2 — Incremental Compilation](part_2_power_user_shell.md) | [Part 4 — Clang Compiler Services →](part_4_embedding_api.md)

