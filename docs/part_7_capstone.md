# Part 7 — A Callable C++ Kernel

[← Part 6 — Advanced JIT Customization](part_6_extending.md) | [README](README.md)

Bring the APIs together in a callable application kernel. Keep source validation, ownership and native execution visible in the host.

## Section 7.1 — Compile a function supplied as a file

Imagine a simulation that loads its calculation rule from a C++ file. The host supplies input numbers, compiles the rule and calls it, so changing the calculation becomes a matter of supplying a different source file.

Begin with a function named `run` that accepts an integer and returns its result. Its source file is input to the embedded compiler; the host's own `main` will load and execute it in Section 7.2. Follow this small kernel through compilation before adding more application behavior around it.

The kernel file defines the calculation independently of the host:

<!-- runnable-example: 7.1 -->

`examples/kernel.cpp`:

```cpp
// C linkage gives the host a stable name; int(int) is the entry interface it validates.
extern "C" int run(int x) { return x * 7; }
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p07_kernel.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p07_kernel
out/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p07_kernel.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p07_kernel
out/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The platform commands compile the host that reads this file at runtime. The reference target provides the same sequence for the recorded run:

```bash
scripts/build.sh p07_kernel
```

```bash
build/bin/p07_kernel manifests/p07_kernel.cpp 0 6 -3
```

```text expected
run(0) = 0
run(6) = 42
run(-3) = -21
```

Follow the file's contents into `Parse`, then through declaration validation and `Execute` to one symbol lookup. The calls for 0, 6 and -3 reuse that address; they do not reread or recompile the file.

Change a copy of the kernel to `return x * x;` and pass that file to the host. Predict all three results before running. Locate the source-loading error branch: the session already exists there, but a missing file stops the host before it submits kernel text to `Parse`.

---

## Section 7.2 — Check the entry signature before calling

Finding a symbol named `run` does not establish that calling it as `int(int)` is valid. The source could define a different return type, accept different parameters or merely declare a function whose body is missing.

Inspect the parsed declaration before executing the module or converting an address to a function pointer. Check the definition, linkage and canonical parameter and return types against the host's required interface. Then try incompatible kernels to see how an ABI assumption becomes an explicit compiler-based check.

Read the validation between `Parse` and `Execute`. It is the point where the host turns its intended calling convention into an explicit rule:

<!-- runnable-example: 7.2 -->

`examples/p07_kernel.cpp`:

```cpp
#include <clang/AST/Decl.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/Type.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
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
  llvm::cl::opt<std::string> SourceFile(
      "source", llvm::cl::desc("C++ source file containing run(int)"),
      llvm::cl::cat(Options), llvm::cl::Required);
  llvm::cl::list<int> InputOptions(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Integer argument for run; repeat for several calls"),
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
  std::vector<int> Inputs(InputOptions.begin(), InputOptions.end());
  if (Inputs.empty()) {
    Inputs = {0, 6, -3};
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
  auto Built = clang::Interpreter::create(std::move(*CompilerOrError), nullptr);
  if (!Built) {
    llvm::logAllUnhandledErrors(Built.takeError(), llvm::errs(), "create: ");
    return 1;
  }
  auto Engine = std::move(*Built);
  std::string SourceText;
  // Read source text into host storage before submitting it to the incremental
  // compiler.
  auto Buffer = llvm::MemoryBuffer::getFileOrSTDIN((SourceFile.getValue()));
  if (!Buffer) {
    llvm::errs() << Buffer.getError().message() << "\n";
    return 1;
  }
  SourceText = (*Buffer)->getBuffer().str();
  // Build the input's AST and IR without running its initializers.
  auto Parsed = Engine->Parse(SourceText);
  if (!Parsed) {
    llvm::logAllUnhandledErrors(Parsed.takeError(), llvm::errs(), "parse: ");
    return 1;
  }
  clang::FunctionDecl *Entry = nullptr;
  // extern "C" declarations can be nested in a LinkageSpecDecl.
  // extern "C" can wrap FunctionDecls in a LinkageSpecDecl; walk through those
  // contexts.
  std::vector<clang::DeclContext *> Contexts{Parsed->TUPart};
  for (size_t Index = 0; Index < Contexts.size(); ++Index) {
    for (clang::Decl *Declaration : Contexts[Index]->decls()) {
      if (auto *Linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(Declaration)) {
        Contexts.push_back(Linkage);
      }
      if (auto *Function = llvm::dyn_cast<clang::FunctionDecl>(Declaration)) {
        if (Function->getNameAsString() == "run" &&
            Function->doesThisDeclarationHaveABody()) {
          Entry = Function;
        }
      }
    }
  }
  // Reject an incompatible definition before executing its module or casting
  // its address.
  // Canonical builtin types accept aliases of int while preserving the required
  // ABI.
  if (!Entry || !Entry->isExternC() || Entry->isVariadic() ||
      Entry->isDeleted() ||
      Entry->getType()->castAs<clang::FunctionProtoType>()->getCallConv() !=
          clang::CC_C ||
      Entry->getNumParams() != 1 ||
      !Entry->getReturnType().getCanonicalType()->isSpecificBuiltinType(
          clang::BuiltinType::Int) ||
      !Entry->getParamDecl(0)
           ->getType()
           .getCanonicalType()
           ->isSpecificBuiltinType(clang::BuiltinType::Int)) {
    llvm::errs() << "entry must be defined as extern \"C\" int run(int)\n";
    return 1;
  }
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error E = Engine->Execute(*Parsed)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "execute: ");
    return 1;
  }
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  auto Address = Engine->getSymbolAddress(clang::GlobalDecl(Entry));
  if (!Address) {
    llvm::logAllUnhandledErrors(Address.takeError(), llvm::errs(), "lookup: ");
    return 1;
  }
  // The validated declaration permits int(int); keep Engine alive through every
  // native call.
  auto Run = Address->toPtr<int (*)(int)>();
  for (int N : Inputs) {
    llvm::outs() << "run(" << N << ") = " << Run(N) << "\n";
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p07_kernel.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p07_kernel
out/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p07_kernel.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p07_kernel
out/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The host walks the parsed declarations, including linkage wrappers, to find `run`. It requires a non-deleted definition with C linkage, the supported calling convention, one non-variadic `int` parameter and an `int` result. Canonical types let an alias of `int` satisfy the same interface.

Only after that validation does it execute the module, resolve the declaration's address and convert it to `int (*)(int)`. The retained interpreter owns the code throughout the input loop. `MemoryBuffer` loads the file, its contents are copied into `SourceText`, and `Parse` receives that text. Each failed operation has a separate error branch.

Make copied kernels with no `run`, a `double run(double)`, a declaration without a body and an ordinary C++ `int run(int)`. Predict the rejection for each, then run them. Also try a typedef of `int` that should pass. Compare accepted language with the narrower interface this application supports.

The signature rule governs calling compatibility. An accepted module can still run global initializers and use process resources, so this validation does not isolate submitted code.

---

## Section 7.3 — Separate compile cost from call cost

Running a calculation through the JIT involves setup followed by native calls. Some work happens once, while each call repeats for another input. Combining those costs into one timing can make a fast native function appear slow, or hide expensive setup behind a large batch of calls.

Measure session creation, parsing, execution, first lookup and repeated native calls separately. Remember that lookup may trigger materialization, so work is not necessarily finished at the point where source parsing ends. Compare batch sizes to explain which cost matters for the workload your host will actually run.

Start by comparing optimization settings with the same source and inputs:

```bash
build/bin/p07_kernel manifests/p07_kernel.cpp 6 7 8 -- -O0
build/bin/p07_kernel manifests/p07_kernel.cpp 6 7 8 -- -O2
```

The final numbers can agree while compilation and machine instructions differ. Measure file loading, compiler creation, parsing, execution and first lookup separately from repeated calls through the resolved pointer. The first lookup may trigger materialization, so placing it inside a timed call loop would mix distinct costs.

Add a repeated-call loop whose accumulated result is printed, and vary the batch size. Keep the source, toolchain and flags beside the measurements so another run can reproduce the workload.

If you later add caching, make its key account for dependencies, compiler version, target, ABI and relevant options. Capturing objects in Part 6 only observes compilation; reusing them requires an implemented cache path and valid keys.

---

## Section 7.4 — Replace a kernel without dangling pointers

A running application may need to replace its kernel without interrupting the version that already works. Overwriting a stored function pointer is not enough, because that address still depends on the interpreter that owns its code.

Prepare the replacement in a second session and switch only after its compilation and lookup succeed. Treat each engine and its callable handles as one lifetime unit, keeping the previous unit alive until its users finish. Try a broken replacement and follow why the old kernel should remain available.

Treat the interpreter and its callable address as one session. Build a replacement in a separate session, validate its entry, execute it and resolve its address before publishing the replacement. A rejected candidate then leaves the working session available.

Implement that swap in a single-threaded host. Run the original kernel, try a broken replacement and call the original again. Then install a valid changed kernel and confirm that the next call uses its result. Discard the old address with its old engine rather than retaining it under the same variable name.

Concurrent callers need an additional lifetime policy: old code must remain alive until its active calls return. Compiler operations on a session also need serialization; LLVM's `ThreadSafeModule` does not make Clang's parser and Sema generally safe for concurrent use.

> [!hint]- When is the old engine safe to destroy?
> Follow every call that could still use its address.

> [!success]- Follow active ownership
> After the replacement is ready and no caller can still execute the old code. In a single-threaded host this is straightforward; concurrent publication and retirement must enforce that condition explicitly.

---

## Section 7.5 — Turn observations into acceptance cases

A kernel that returns the right number once has exercised only one path through the host. A wrong signature, invalid source and an unresolved symbol fail at different stages, and each should produce an outcome the application can recognize.

Turn those boundaries into small acceptance cases with known inputs and observable results. Include repeated calls and malformed host arguments as well as compiler and linker failures. For each case, identify the stage being exercised and the behavior that would distinguish success from a regression.

Run the kernel's acceptance cases, then relate each rejection to a particular stage in the host:

```bash
scripts/test_kernel.sh
```

A missing file belongs to loading, invalid C++ belongs to diagnostics, an incompatible entry belongs to signature validation, and a missing runtime definition belongs to lookup or linking. Keeping those cases distinct helps the host report a useful error instead of attempting a native call with an invalid assumption.

Add a typedef-of-`int` signature and an out-of-range command-line integer. Predict which one should pass: canonical type comparison accepts the alias, while integer parsing rejects a value outside its range. Preserve concise public messages alongside any detailed Clang diagnostic.

---

## Section 7.6 — Extend the host in one direction

The callable kernel now brings source loading, compiler checks, symbol lookup and native calls together. Choose one additional need for that application: calling a host service, inspecting declarations before execution, saving object code or using another executor.

Carry the relevant API from an earlier part into a copy of the host and follow one input through the extended path. Add a failure case for the new boundary and explain who owns the added state. This turns an isolated API experiment into an application feature whose behavior and lifetime you can account for.

Choose one extension and keep the kernel interface fixed while implementing it. For example, add diagnostic collection, object capture or a host callback. Follow the added behavior through one successful call and one failure, including the lifetime of every retained address or result.

If you want a prompt, `tools/p07_xrepl/` applies the same engine behind input editing and commands; `scripts/test_xrepl.sh` exercises that frontend. Read it after the callable host so you can identify which behavior belongs to presentation and which belongs to the compiler session.

Finish by tracing source text into an AST, a module, a runtime symbol and a native result in your extension. Name the API at each transition and show how errors return to the host. That trace should still make sense after changing the frontend or replacing the session.

---

[← Part 6 — Advanced JIT Customization](part_6_extending.md) | [README](README.md)

