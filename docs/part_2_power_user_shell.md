# Part 2 — Incremental Compilation

[← Part 1 — Your First Embedded JIT](part_1_shell_basics.md) | [Part 3 — Connecting Host and JIT →](part_3_under_the_hood.md)

Keep the same engine alive while you separate parsing, AST construction, IR generation and execution. The fixtures are submitted by native API tools, so no shell commands are needed inside the interpreter.

```dot
digraph compiler_state {
  rankdir=LR;
  source [label="source input", class="data"];
  parse [label="Parse", class="api"];
  ptu [label="PTU: AST + LLVM module", class="data"];
  inspect [label="inspect declarations / IR", class="api"];
  execute [label="Execute", class="api"];
  runtime [label="linked code + initializers", class="data"];
  source -> parse -> ptu;
  ptu -> inspect;
  ptu -> execute -> runtime;
}
```

## Section 2.1 — `Parse` versus `Execute`

Suppose your host wants to inspect a function before allowing its initializers to run. A single `ParseAndExecute` call moves through both compilation and execution, so it gives you no place to make that decision between them.

Split the operation into `Parse` and `Execute`. Parsing returns a `PartialTranslationUnit` containing the accepted declarations and any generated module; execution submits that module and runs its initialization code. Follow a printing statement and a function definition through the two calls, predicting which visible effects can happen before `Execute`.

<!-- runnable-example: 2.1 -->

`examples/p04_parse.cpp`:

```cpp
#include <clang/AST/Decl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// ORC lists the symbols that failed to materialize in hash order; keep the
// message but drop the set so the output is the same on every run.

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
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
#include <cstdio>
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  llvm::outs() << "== 1. a statement that prints\n";
  // Build the input's AST and IR without running its initializers.
  auto ParsedInputOrError = I->Parse(R"cpp(
printf("hello from the JIT\n");
)cpp");
  if (!ParsedInputOrError) {
    llvm::logAllUnhandledErrors(ParsedInputOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // The PTU is interpreter-owned: inspect its AST and module before execution.
  PartialTranslationUnit &S = (*ParsedInputOrError);
  {
    auto &&WhenArgument = ("after Parse  ");
    auto &&PTUArgument = (S);

    unsigned Decls = std::distance(PTUArgument.TUPart->decls_begin(),
                                   PTUArgument.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument.TheModule) {
      llvm::outs() << " name=" << PTUArgument.TheModule->getName();
    }
    llvm::outs() << "\n";
  }
  llvm::outs() << "  (nothing has run yet)\n";
  // Submit the PTU module to the executor and run its initialization code.
  if (llvm::Error Error = I->Execute(S)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  {
    auto &&WhenArgument2 = ("after Execute");
    auto &&PTUArgument2 = (S);

    unsigned Decls = std::distance(PTUArgument2.TUPart->decls_begin(),
                                   PTUArgument2.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument2 << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument2.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument2.TheModule) {
      llvm::outs() << " name=" << PTUArgument2.TheModule->getName();
    }
    llvm::outs() << "\n";
  }

  llvm::outs() << "== 2. a definition, parsed but not executed\n";
  auto ParsedInputOrError2 = I->Parse(R"cpp(
int triple(int v) {
  return 3 * v;
}
)cpp");
  if (!ParsedInputOrError2) {
    llvm::logAllUnhandledErrors(ParsedInputOrError2.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // Keep this definition unexecuted to separate AST visibility from runtime
  // availability.
  PartialTranslationUnit &F = (*ParsedInputOrError2);
  {
    auto &&WhenArgument3 = ("after Parse  ");
    auto &&PTUArgument3 = (F);

    unsigned Decls = std::distance(PTUArgument3.TUPart->decls_begin(),
                                   PTUArgument3.TUPart->decls_end());
    llvm::outs() << "  " << WhenArgument3 << ": TUPart decls=" << Decls
                 << " TheModule non-null="
                 << ((PTUArgument3.TheModule != nullptr) ? "yes" : "no");
    if (PTUArgument3.TheModule) {
      llvm::outs() << " name=" << PTUArgument3.TheModule->getName();
    }
    llvm::outs() << "\n";
  }
  llvm::outs() << "  getSymbolAddress(_Z6triplei): ";
  // Resolve a runtime address; lookup may compile/link a module on demand.
  // The session owns code/storage; function-pointer types must match the ABI.
  if (auto Addr = I->getSymbolAddress("_Z6triplei")) {
    llvm::outs() << "found\n";
  } else {
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  }
  llvm::outs() << "  a later ParseAndExecute(\"triple(14)\"):\n";
  if (llvm::Error E = I->ParseAndExecute(R"cpp(
triple(14)
)cpp")) {
    llvm::outs() << "  error: " << (llvm::toString((std::move(E)))) << "\n";
  }
  llvm::outs() << "  Execute the pending definition:\n";
  if (llvm::Error E = I->Execute(F)) {
    llvm::outs() << "  error: " << (llvm::toString((std::move(E)))) << "\n";
  }
  llvm::outs() << "  getSymbolAddress(_Z6triplei) now: ";
  if (auto Addr = I->getSymbolAddress("_Z6triplei")) {
    llvm::outs() << "found\n";
  } else {
    llvm::outs() << llvm::toString(Addr.takeError()) << "\n";
  }
  llvm::outs() << "  ParseAndExecute(\"triple(14)\") again:\n";
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
triple(14)
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  llvm::outs() << "== 3. parse two inputs, then execute them in order\n";
  auto ParsedInputOrError3 = I->Parse(R"cpp(
int quad(int v) {
  return 4 * v;
}
)cpp");
  if (!ParsedInputOrError3) {
    llvm::logAllUnhandledErrors(ParsedInputOrError3.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  PartialTranslationUnit &D = (*ParsedInputOrError3);
  auto ParsedInputOrError4 = I->Parse(R"cpp(
printf("quad(5) = %d\n", quad(5));
)cpp");
  if (!ParsedInputOrError4) {
    llvm::logAllUnhandledErrors(ParsedInputOrError4.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  PartialTranslationUnit &U = (*ParsedInputOrError4);
  // Execute the dependency before its caller; parsing both does not register
  // either definition.
  if (llvm::Error Error = I->Execute(D)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  if (llvm::Error Error = I->Execute(U)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_parse.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_parse
out/p04_parse -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_parse.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_parse
out/p04_parse -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

`Parse` returns a reference to the session's `PartialTranslationUnit`. Its `TUPart` holds this input's declarations; `TheModule` holds its generated LLVM IR. The reference belongs to the interpreter and must not be retained across undoing that input or destroying the session.

The reference tool separates three cases so you can see when compilation becomes execution:

```bash
build/bin/p04_parse
```

```text expected
== 1. a statement that prints
  after Parse  : TUPart decls=1 TheModule non-null=yes name=incr_module_2
  (nothing has run yet)
hello from the JIT
  after Execute: TUPart decls=1 TheModule non-null=no
== 2. a definition, parsed but not executed
  after Parse  : TUPart decls=1 TheModule non-null=yes name=incr_module_3
  getSymbolAddress(_Z6triplei): Symbols not found: [ __Z6triplei ]
  a later ParseAndExecute("triple(14)"):
JIT session error: Symbols not found: [ __Z6triplei ]
  error: Failed to materialize symbols: { ... }
  Execute the pending definition:
  error: Failed to materialize symbols: { ... }
  getSymbolAddress(_Z6triplei) now: found
  ParseAndExecute("triple(14)") again:
(int) 42
== 3. parse two inputs, then execute them in order
quad(5) = 20
```

In the first case, `Parse` creates a wrapper for the printing statement, but `hello from the JIT` appears only after `Execute`. Execution moves `TheModule` into the JIT, leaving the PTU's module pointer null. Inspect IR before that transfer.

In the second case, parsing `triple` makes its declaration visible to Clang without giving its definition to the executor. A later caller therefore type-checks but fails to link. In LLVM 22.1.8 that failure can leave a registered initializer that reports the error again on the next JIT operation. Recreate the session if recovery becomes unreliable.

The third case executes the definition before its caller, so `quad(5)` runs and returns 20. Preserve that dependency order when collecting several PTUs before executing them.

Predict which module numbers the tool prints, then extract them:

```bash
build/bin/p04_parse 2>&1 | grep -o 'incr_module_[0-9]*' | sort -u | tr '\n' ' '
```

```text expected
incr_module_2 incr_module_3
```

The initial header was input 1, so the two PTUs described by the tool are `incr_module_2` and `incr_module_3`. Module numbering tracks accepted inputs; the `input_line_N` source-buffer names used in diagnostics also count rejected inputs.

> [!hint]- Can the second PTU execute before the first?
> Ask whether its code needs a definition from the first module.

> [!success]- Follow the dependency
> An independent second module can run. A module that calls a function from the first needs that definition added to the JIT first; a visible AST declaration is insufficient.

Change the standalone example's `quad` body to multiply by five. Rebuild and predict the result while keeping the definition's `Execute` before the caller's.

---

## Section 2.2 — Inspecting a PTU's AST

Your host may need to discover what an input declared: a variable, a function, a record or a namespace. The text alone is a poor guide because Clang has already resolved language details such as types and declaration structure. Its abstract syntax tree gives you that meaning directly.

Start from the input's `TUPart` and inspect its declarations. Compare a variable definition, a function and a declaration nested inside a namespace; notice which names appear at the top level and which require walking into another declaration context. The AST describes the program's language structure, while runtime values live in the executing program.

`TUPart` is a `TranslationUnitDecl`, so its `decls()` range lets the host examine what one input declared. `getDeclKindName()` identifies the node, and `NamedDecl::getQualifiedNameAsString()` supplies its name when it has one.

<!-- runnable-example: 2.2 -->

`examples/p04_ast.cpp`:

```cpp
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int base = 40;
)cpp",
    R"cpp(
int add(int v) {
  return v + base;
}
)cpp",
    R"cpp(
base = add(2);
)cpp",
    R"cpp(
base
)cpp",
    R"cpp(
namespace lab {
struct Box {
  int w;
};
int area(Box b) {
  return b.w * b.w;
}
} // namespace lab
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::list<unsigned> DumpNumbers(
      "dump", llvm::cl::ZeroOrMore,
      llvm::cl::desc(
          "Input number whose AST should be dumped; repeat for several inputs"),
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

  std::set<unsigned> DumpInputs(DumpNumbers.begin(), DumpNumbers.end());

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

  std::vector<TranslationUnitDecl *> TUs;
  unsigned N = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    llvm::outs() << "[" << N << "] " << Code << "\n";
    // Build the input's AST and IR without running its initializers.
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::outs() << "    parse failed: " << llvm::toString(PTU.takeError())
                   << "\n";
      continue;
    }
    TUs.push_back(PTU->TUPart);
    if (PTU->TUPart->decls_empty()) {
      llvm::outs() << "    (no declarations)\n";
    }
    // Visit only this input's declarations; earlier inputs have their own TU
    // parts.
    for (Decl *D : PTU->TUPart->decls()) {
      llvm::outs() << "    " << D->getDeclKindName();
      // dyn_cast tests the AST node kind and returns null for declarations
      // without names.
      if (auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
        llvm::outs() << " " << ND->getQualifiedNameAsString();
      }
      llvm::outs() << "\n";
      if (DumpInputs.count(N)) {
        std::string ASTDump;
        llvm::raw_string_ostream OutputForASTDump(ASTDump);
        D->dump(OutputForASTDump);
        llvm::outs() << ASTDump;
      }
    }
    // Syntax-only inputs can have no module; inspect declarations before
    // transferring IR.
    if (PTU->TheModule) {
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(*PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
      }
    }
  }

  if (TUs.size() >= 2) {
    TranslationUnitDecl *Prev = TUs[TUs.size() - 2], *Last = TUs.back();
    llvm::outs() << "---\n";
    llvm::outs() << "last two inputs have the same TranslationUnitDecl: "
                 << (Prev == Last ? "yes" : "no") << "\n";
    llvm::outs() << "their getCanonicalDecl() are the same: "
                 << (Prev->getCanonicalDecl() == Last->getCanonicalDecl()
                         ? "yes"
                         : "no")
                 << "\n";
    llvm::outs() << "ASTContext::getTranslationUnitDecl() is the last input's: "
                 << (I->getASTContext().getTranslationUnitDecl() == Last ? "yes"
                                                                         : "no")
                 << "\n";
    llvm::outs() << "the canonical decl is one of the inputs' TUs: "
                 << (llvm::is_contained(TUs, Last->getCanonicalDecl()) ? "yes"
                                                                       : "no")
                 << "\n";
  }
  return 0;
}
```

Linux:

```bash
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_ast.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_ast
out/p04_ast --dump=4 --dump=5 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_ast.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_ast
out/p04_ast --dump=4 --dump=5 -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The reference fixture includes declarations, a statement and an expression:

```bash
cat manifests/p04_ast.repl
build/bin/p04_ast manifests/p04_ast.repl
```

```text expected
// Part 4.6 -- one declaration, a function, a statement, an expression, a namespace.
int base = 40;
int add(int v) { return v + base; }
base = add(2);
base
namespace lab { struct Box { int w; }; int area(Box b) { return b.w * b.w; } }
[1] // Part 4.6 -- one declaration, a function, a statement, an expression, a namespace.
    (no declarations)
[2] int base = 40;
    Var base
[3] int add(int v) { return v + base; }
    Function add
[4] base = add(2);
    TopLevelStmt
[5] base
    TopLevelStmt
[6] namespace lab { struct Box { int w; }; int area(Box b) { return b.w * b.w; } }
    Namespace lab
---
last two inputs have the same TranslationUnitDecl: no
their getCanonicalDecl() are the same: yes
ASTContext::getTranslationUnitDecl() is the last input's: yes
the canonical decl is one of the inputs' TUs: no
```

The comment creates no declarations; the variable, function and namespace create their corresponding AST nodes. A top-level statement appears as `TopLevelStmtDecl`, which carries the ordinary statement subtree. Inspect the assignment to `base`:

```bash
build/bin/p04_ast manifests/p04_ast.repl 4 | sed -n '/^\[4\]/,/^\[5\]/{/^\[5\]/!p;}'
```

```text expected
[4] base = add(2);
    TopLevelStmt
TopLevelStmtDecl 0xADDR <input_line_4:1:1, col:13> col:1
`-BinaryOperator 0xADDR <col:1, col:13> 'int' lvalue '='
  |-DeclRefExpr 0xADDR <col:1> 'int' lvalue Var 0xADDR 'base' 'int'
  `-CallExpr 0xADDR <col:8, col:13> 'int'
    |-ImplicitCastExpr 0xADDR <col:8> 'int (*)(int)' <FunctionToPointerDecay>
    | `-DeclRefExpr 0xADDR <col:8> 'int (int)' lvalue Function 0xADDR 'add' 'int (int)'
    `-IntegerLiteral 0xADDR <col:12> 'int' 2
```

Its `BinaryOperator` contains a reference to `base` and a call to `add`. Source locations point into `input_line_4`, the buffer Clang parsed. Compare the expression without a semicolon:

```bash
build/bin/p04_ast manifests/p04_ast.repl 5 | sed -n '/^\[5\]/,/^\[6\]/{/^\[6\]/!p;}' | cut -c1-110
```

```text expected
[5] base
    TopLevelStmt
TopLevelStmtDecl 0xADDR <<invalid sloc>, input_line_5:1:1> <invalid sloc>
`-CallExpr 0xADDR <<invalid sloc>, col:1> 'void'
  |-ImplicitCastExpr 0xADDR <<invalid sloc>> 'void (*)(void *, void *, void *, ...)' <FunctionToPointerDecay>
  | `-DeclRefExpr 0xADDR <<invalid sloc>> 'void (void *, void *, void *, ...)' lvalue Function 0xADDR '__clang
  |-CStyleCastExpr 0xADDR <<invalid sloc>> 'void *' <IntegralToPointer>
  | `-IntegerLiteral 0xADDR <<invalid sloc>> 'unsigned long long' <addr>
  |-CStyleCastExpr 0xADDR <<invalid sloc>> 'void *' <IntegralToPointer>
  | `-IntegerLiteral 0xADDR <<invalid sloc>> 'unsigned long long' <addr>
  |-CStyleCastExpr 0xADDR <<invalid sloc>> 'void *' <IntegralToPointer>
  | `-IntegerLiteral 0xADDR <<invalid sloc>> 'unsigned long long' <addr>
  `-CStyleCastExpr 0xADDR <<invalid sloc>, col:1> 'unsigned _BitInt(64)' <NoOp>
    `-ImplicitCastExpr 0xADDR <col:1> 'unsigned _BitInt(64)' <IntegralCast> part_of_explicit_cast
      `-ImplicitCastExpr 0xADDR <col:1> 'int' <LValueToRValue> part_of_explicit_cast
        `-DeclRefExpr 0xADDR <col:1> 'int' lvalue Var 0xADDR 'base' 'int'
```

The interpreter has rewritten it into a call to `__clang_Interpreter_SetValueNoAlloc`. That generated call carries the interpreter, result storage, type and expression value. Synthesized nodes have no source location, while nodes retained from the user's expression can keep theirs.

Each input has a separate translation-unit part, but their redeclaration chain joins the session. `getCanonicalDecl()` reaches their common canonical translation unit; `ASTContext::getTranslationUnitDecl()` reaches the latest part. This is why iterating only the latest part does not list the whole session.

Predict the top-level declarations created by a record definition followed by a variable of that type:

```bash
mkdir -p out
printf '%s\n' 'struct P { int a; }; P p;' > out/p04_verify.repl
build/bin/p04_ast out/p04_verify.repl | head -n 4
```

```text expected
[1] struct P { int a; }; P p;
    CXXRecord P
    Var p
```

The two top-level nodes are the record and variable. Implicit declarations inside the record do not become additional entries in the translation unit's top-level range.

> [!hint]- Why do the assignment and capture call have different locations?
> One was parsed from text; the other was constructed by the interpreter.

> [!success]- Read the source locations
> Parsed nodes point into the input buffer. Generated nodes have invalid locations unless they retain a source node with a real location.

Add `using Count = unsigned;` to a copy of the AST fixture and inspect its node. Compare the alias with an expression statement: both belong to an input, but only one introduces a named type.

---

## Section 2.3 — Inspecting a PTU's LLVM IR

The AST tells you what C++ means, but the JIT receives a lower-level representation: LLVM IR. Looking at that representation helps explain how a function, a global variable or a top-level statement becomes work the executor can perform.

Print the PTU's module before executing it. Find the generated function and the code associated with initialization, then connect those definitions to the source that produced them. Keep compilation and execution separate while exploring: printing a module lets you examine code before its initializers run.

Before execution, `PTU.TheModule` is an ordinary `llvm::Module`. Calling `print` reveals the code that the executor will receive.

<!-- runnable-example: 2.3 -->

`examples/p04_ir.cpp`:

```cpp
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Raw addresses show up as big decimal constants (inttoptr (i64 38403440896
// ...)).

// Drop the boilerplate every module carries (target, flags, attribute groups).

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int counter = 5;
)cpp",
    R"cpp(
int bump(int by) {
  counter += by;
  return counter;
}
)cpp",
    R"cpp(
bump(10);
)cpp",
    R"cpp(
counter
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> LL("ll",
                                llvm::cl::desc("Write LLVM IR to this file"),
                                llvm::cl::cat(Options), llvm::cl::init(""));
  llvm::cl::opt<bool> Full("full", llvm::cl::desc("Print complete LLVM IR"),
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

  std::unique_ptr<llvm::raw_fd_ostream> File;
  if (!LL.empty()) {
    std::error_code EC;
    File = std::make_unique<llvm::raw_fd_ostream>(LL, EC);
    if (EC) {
      llvm::errs() << "ir: cannot open " << LL << ": " << EC.message() << "\n";
      return 1;
    }
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
  unsigned N = 0;
  for (const std::string &Code : SourceInputs) {
    ++N;
    // Build the input's AST and IR without running its initializers.
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
    if (!PTU) {
      llvm::logAllUnhandledErrors(PTU.takeError(), llvm::errs(), "error: ");
      continue;
    }
    std::string IR;
    llvm::raw_string_ostream IROS(IR);
    // An llvm::Module is the generated IR, not running code. Print it before
    // Execute.
    if (PTU->TheModule) { // a syntax-only run (-fsyntax-only) generates no code
      // The stream receives textual LLVM IR; nullptr requests no annotation
      // writer.
      PTU->TheModule->print(IROS, nullptr);
    }
    if (File) {
      *File << "; ---- input " << N << ": " << Code << "\n" << IR;
    } else {
      llvm::outs() << "; ---- input " << N << ": " << Code << "\n";
      llvm::outs() << IR;
    }
    if (PTU->TheModule) {
      // Submit the PTU module to the executor and run its initialization code.
      if (llvm::Error Error = I->Execute(*PTU)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
        return 1;
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
  examples/p04_ir.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_ir
out/p04_ir -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_ir.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_ir
out/p04_ir -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

Start with the global and the function in the reference fixture:

```bash
cat manifests/p04_ir.repl
build/bin/p04_ir manifests/p04_ir.repl | sed -n '/input 2/,/input 4/{/input 4/!p;}'
```

```text expected
// Part 4.7 -- a global, a function, a statement and an expression.
int counter = 5;
int bump(int by) { counter += by; return counter; }
bump(10);
counter
; ---- input 2: int counter = 5;
@counter = global i32 5, align 4

; ---- input 3: int bump(int by) { counter += by; return counter; }
@counter = external global i32, align 4

; Function Attrs: mustprogress noinline nounwind optnone ssp uwtable(sync)
define noundef i32 @_Z4bumpi(i32 noundef %0) #0 {
  %2 = alloca i32, align 4
  store i32 %0, ptr %2, align 4
  %3 = load i32, ptr %2, align 4
  %4 = load i32, ptr @counter, align 4
  %5 = add nsw i32 %4, %3
  store i32 %5, ptr @counter, align 4
  %6 = load i32, ptr @counter, align 4
  ret i32 %6
}
```

The global's module defines `counter`. The function's module refers to it as an external global because the definition belongs to an earlier input. ORC resolves that dependency when linking the generated code. At the default optimization setting, `optnone` and stack traffic make the function's operations easy to follow.

Now inspect the statement that calls the function:

```bash
build/bin/p04_ir manifests/p04_ir.repl | sed -n '/input 4/,/input 5/{/input 5/!p;}'
```

```text expected
; ---- input 4: bump(10);
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_incr_module_4, ptr null }]

define internal void @__stmts__0() {
  %1 = call noundef i32 @_Z4bumpi(i32 noundef 10)
  ret void
}

declare noundef i32 @_Z4bumpi(i32 noundef) #0

; Function Attrs: noinline ssp uwtable(sync)
define internal void @_GLOBAL__sub_I_incr_module_4() #1 section "__TEXT,__StaticInit,regular,pure_instructions" {
  call void @__stmts__0()
  ret void
}
```

Clang puts the statement in `__stmts__0`, then registers an initializer through `llvm.global_ctors` to call it. `Execute` runs that initialization, which explains why parsing alone had no printing effect. A bare expression uses the same route but adds result capture:

```bash
build/bin/p04_ir manifests/p04_ir.repl | sed -n '/input 5/,$p' | grep -E 'define|call|global_ctors' | cut -c1-120
```

```text expected
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_incr_m
define internal void @__stmts__0() {
  call void (ptr, ptr, ptr, ...) @__clang_Interpreter_SetValueNoAlloc(ptr noundef inttoptr (i64 <addr> to ptr), ptr noun
define internal void @_GLOBAL__sub_I_incr_module_5() #1 section "__TEXT,__StaticInit,regular,pure_instructions" {
  call void @__stmts__0()
```

Follow the load of `counter`, the conversion and the call to `__clang_Interpreter_SetValueNoAlloc`. The masked constants include addresses used by that capture machinery.

Predict what will remain of the function's stack slots at `-O2`:

```bash
build/bin/p04_ir manifests/p04_ir.repl -- -O2 | sed -n '/input 3/,/input 4/{/input 4/!p;}' | grep -v '^$'
```

```text expected
; ---- input 3: int bump(int by) { counter += by; return counter; }
@counter = external local_unnamed_addr global i32, align 4
; Function Attrs: mustprogress nofree norecurse nosync nounwind ssp willreturn memory(readwrite, argmem: none, inaccessiblemem: none, target_mem0: none, target_mem1: none) uwtable(sync)
define noundef i32 @_Z4bumpi(i32 noundef %0) local_unnamed_addr #0 {
  %2 = load i32, ptr @counter, align 4, !tbaa !6
  %3 = add nsw i32 %2, %0
  store i32 %3, ptr @counter, align 4, !tbaa !6
  ret i32 %3
}
```

Optimization has already happened in Clang's CodeGen path during `Parse`. The reduced IR contains the load, arithmetic, store and return that implement the function; `Execute` then submits that module to the executor.

Save the unfiltered IR when you want to inspect it with other LLVM tools:

```bash
mkdir -p out
build/bin/p04_ir manifests/p04_ir.repl --ll=out/p04_last.ll
grep -E '^define|^declare|^@llvm.global_ctors' out/p04_last.ll | cut -c1-100
```

```text expected
define noundef i32 @_Z4bumpi(i32 noundef %0) #0 {
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_
define internal void @__stmts__0() {
declare noundef i32 @_Z4bumpi(i32 noundef) #0
define internal void @_GLOBAL__sub_I_incr_module_4() #1 section "__TEXT,__StaticInit,regular,pure_in
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_
define internal void @__stmts__0() {
declare void @__clang_Interpreter_SetValueNoAlloc(ptr noundef, ptr noundef, ptr noundef, ...) #0
define internal void @_GLOBAL__sub_I_incr_module_5() #1 section "__TEXT,__StaticInit,regular,pure_in
```

The reference tool's `--full` option keeps the module details normally omitted from its display. Use it to locate the initializer registration in each input:

```bash
build/bin/p04_ir manifests/p04_ir.repl --full | grep -E '^; ---- input|@llvm.global_ctors' | cut -c1-70
```

```text expected
; ---- input 1: // Part 4.7 -- a global, a function, a statement and a
; ---- input 2: int counter = 5;
; ---- input 3: int bump(int by) { counter += by; return counter; }
; ---- input 4: bump(10);
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32,
; ---- input 5: counter
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32,
```

In this fixture, the statement and expression inputs have `global_ctors` entries; the simple constant global and function definition do not. A declaration with dynamic initialization can also register initialization code, as Part 3 demonstrates.

> [!hint]- How does a new module call `bump` from an older input?
> Find where the earlier definition entered the JIT.

> [!success]- Follow the external reference
> ORC resolves the caller's undefined symbol against the executed definition. If that definition had only been parsed, its AST would be visible but its runtime symbol would be missing.

In a copied IR fixture, replace `counter += by` with `counter *= by`. Locate the multiplication in the emitted IR, then follow how its result returns to the caller.

---

## Section 2.4 — Choose language and optimization settings

The compiler inside your application has its own settings. Changing the flags used to build the host does not automatically change the language mode or optimization level used for a function compiled later by the JIT.

Configure those choices on `IncrementalCompilerBuilder` before creating the session. Pass compiler flags after `--` in the example commands, then compare a language setting with an optimization setting. Notice whether each change affects which source Clang accepts or how accepted source is compiled.

<!-- runnable-example: 2.4 -->

`examples/p04_hello.cpp`:

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
  llvm::cl::list<std::string> InputsOption(
      "input", llvm::cl::ZeroOrMore,
      llvm::cl::desc("C++ input to compile; repeat to submit more inputs"),
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
  std::setvbuf(stdout, nullptr, _IONBF,
               0); // keep JIT printf and llvm::outs() in order
  llvm::outs().SetUnbuffered();

  // 1. Builder: turns driver-style arguments into a CompilerInstance.
  std::vector<std::string> All;
  All.insert(All.end(), CompilerFlags.begin(), CompilerFlags.end());
  std::vector<const char *> Argv;
  for (const std::string &S : All) {
    Argv.push_back(S.c_str());
  }
  clang::IncrementalCompilerBuilder Builder;
  // Configure the embedded compiler, independently of the host build flags.
  // These argument strings must remain alive until CreateCpp() finishes.
  Builder.SetCompilerArgs(Argv);
  // CreateCpp returns an owned CompilerInstance or an error inside Expected.
  auto CompilerOrError = Builder.CreateCpp();
  // Extract the failed Expected with takeError(); logging consumes that Error.
  if (!CompilerOrError) {
    llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<clang::CompilerInstance> CI = std::move(*CompilerOrError);
  // An embedding host loads requested frontend plugins explicitly, as the
  // clang-repl driver does; the builder alone does not load their libraries.
  // Load requested plugin libraries before the frontend starts parsing the
  // session.
  CI->LoadRequestedPlugins();
  if (CI->getDiagnostics().hasErrorOccurred()) {
    return 1;
  }

  // 2. Interpreter: takes ownership of the CompilerInstance.
  // Give the session the compiler and prepare its runtime prelude.
  auto InterpreterOrError = clang::Interpreter::create(std::move(CI));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<clang::Interpreter> Interp = std::move(*InterpreterOrError);

  std::vector<std::string> Inputs(InputsOption.begin(), InputsOption.end());
  if (Inputs.empty()) {
    Inputs = {
        R"cpp(
int answer = 41;
)cpp",
        R"cpp(
#include <cstdio>
printf("statement: answer + 1 = %d\n", answer + 1);
)cpp",
        R"cpp(
answer * 2
)cpp",
    };
  }

  // 3. Feed it code. Each call is one input, one PTU.
  for (const std::string &Code : Inputs) {
    llvm::outs() << "input: "
                 << llvm::StringRef(Code).trim().take_until(
                        [](char C) { return C == '\n'; })
                 << "\n";
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error E = Interp->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(E), llvm::outs(), "  error: ");
      return 1;
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
  examples/p04_hello.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -Wl,--export-dynamic -lclang-cpp -lLLVM -o out/p04_hello
out/p04_hello -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" --gcc-toolchain=/usr
```

macOS:

```bash
export LLVM="${LLVM:-$(brew --prefix llvm)}"
mkdir -p out
"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \
  examples/p04_hello.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \
  -lclang-cpp -lLLVM -o out/p04_hello
out/p04_hello -- -std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)" -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"
```

<!-- /runnable-example -->

The builder receives language, macro and optimization options before `CreateCpp`. The generated function uses `FACTOR`, so changing its definition changes the compiled body even though the host's source and function-pointer type stay the same.

The reference tool exposes that same compiler-flag path:

```bash
build/bin/p04_hello 'int scale(int x) { return x * FACTOR; }' 'scale(6)' -- -std=c++20 -DFACTOR=7 -O2
```

```text expected
input: int scale(int x) { return x * FACTOR; }
input: scale(6)
(int) 42
```

Change `FACTOR` to 9 and predict the result. Then compare the function's IR at two optimization levels:

```bash
build/bin/p04_ir manifests/p04_ir.repl -- -O0
build/bin/p04_ir manifests/p04_ir.repl -- -O2
```

Find a removed stack slot or a changed attribute and explain the computation it represented. To study performance, measure compiler construction, parsing, first lookup and repeated calls separately; a smaller IR listing alone does not measure application speed.

> [!hint]- Which `-O2` controls generated code?
> The host build and embedded compiler have different invocations.

> [!success]- Follow the option
> The option passed through `IncrementalCompilerBuilder` controls runtime source compilation. Optimizing the host does not change an already configured frontend.

---

## Section 2.5 — Give the engine headers

A source string containing `#include <vector>` asks the embedded compiler to find a real header file. It does not inherit the host compiler's earlier include search simply because both compilers run on the same machine. Missing or mismatched paths can therefore stop an otherwise valid input before its declarations are parsed.

Follow the resource-header, C++ library and operating-system search paths in the compiler arguments. Include a standard header, create an object from it and evaluate an expression using that object. Connect a missing-header diagnostic to compiler configuration before looking for a problem in the JIT linker.

The incremental compiler must parse a header before it can use the declarations inside it. Including `<vector>` in the host's source does not include it in the session.

```bash
build/bin/p04_hello '#include <vector>' 'std::vector<int> samples{3, 4, 5};' 'int total = 0; for (int n : samples) total += n;' 'total'
```

```text expected
input: #include <vector>
input: std::vector<int> samples{3, 4, 5};
input: int total = 0; for (int n : samples) total += n;
input: total
(int) 12
```

Follow the inputs in order: the header makes `std::vector` known, the declaration creates `samples`, and the loop computes the sum. Remove one embedded header-search option from a standalone run, observe which include fails, then restore it. This connects the search path to the compiler that actually consumes the header.

Create `settings.h` with `constexpr int multiplier = 7;`. Give Section 2.4's host its directory using `-I`, include the header in the JIT source and evaluate `multiplier * 6`. Rebuild and explain where the declaration enters the AST.

A precompiled header can retain a larger common prelude:

```bash
mkdir -p out
"$LLVM/bin/clang++" -std=c++20 -x c++-header \
  -Xclang -fincremental-extensions -Xclang -emit-pch \
  manifests/p02_prelude.h -o out/p02_prelude.pch
build/bin/p04_hello 'show(std::vector<int>{1, 2, 3});' -- \
  -std=c++20 -include-pch out/p02_prelude.pch
```

The PCH needs compatible language options, including `-fincremental-extensions`, because it will be loaded by an incremental frontend. Compiler version, target and relevant macros must also agree. It saves compiler input state rather than reusable machine code.

Choose an expression using one declaration from the prelude and compare a fresh session with and without the PCH. Include loading time and memory in that comparison, since the saved parsing work has its own cost.

---

## Section 2.6 — Type-check without executing

Imagine a tool that checks a C++ snippet and lists its declarations before anyone runs it. It needs Clang's parsing and type rules, but producing machine code would add work without helping that particular request.

Create the session with `-fsyntax-only`. Submit valid and invalid declarations and inspect the resulting AST and diagnostics. The session still remembers accepted declarations between inputs, while its PTUs have no generated LLVM module. Use that difference to recognize which parts of the interpreter are compiler services and which require an executor.

A syntax-only frontend still parses and type-checks each input, but never creates a module for the executor. The reference checker prints both the accepted declarations and the absence of IR:

```bash
build/bin/p06_check manifests/p06_snippets.repl
```

```text expected
execution engine: Operation failed. No code generator available
[1] #include <vector>
    accepted; declares: nothing; has IR: no
[2] struct Point { int x, y; };
    accepted; declares: CXXRecord Point; has IR: no
[3] int area(Point p) { return p.x * p.y; }
    accepted; declares: Function area; has IR: no
[4] int broken(Point p) { return p.z; }
    rejected
    1:32: error: no member named 'z' in 'Point'
[5] int uses_area() { return area(Point{2, 3}); }
    accepted; declares: Function uses_area; has IR: no
[6] auto v = std::vector<int>{1, 2, 3};
    accepted; declares: Var v; has IR: no
[7] undeclared_function(1);
    rejected
    1:1: error: use of undeclared identifier 'undeclared_function'
[8] template <class T> T twice(T v) { return v + v; }
    accepted; declares: FunctionTemplate twice; has IR: no
[9] double d = twice(1.5);
    accepted; declares: Var d; has IR: no
ParseAndExecute("int tail_decl = 1;"): ok
```

Copy the fixture to `out/p02_check.repl` and add `int later = 9;`, followed by `later += 1;` as a separate input. The compiler can validate that assignment using the earlier declaration; it does not execute a memory write.

Find where a code-generating host would inspect `TheModule`, and explain why that branch must handle null here. Section 6.8 builds this configuration directly with the C++ APIs and collects its diagnostics.

---

[← Part 1 — Your First Embedded JIT](part_1_shell_basics.md) | [Part 3 — Connecting Host and JIT →](part_3_under_the_hood.md)

