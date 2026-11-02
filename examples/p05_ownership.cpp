#include <clang/AST/ASTContext.h>
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

// Host-side counter the interpreted destructor increments: lets the tool
// observe "the destructor ran" without touching an object that may already be
// gone.
extern "C" int destructorCount = 0;

// An object with a visible life cycle. The copy constructor prints too, so the
// copy that SetValueWithAlloc makes into the Value's storage is visible.
static const char *Prelude = R"cpp(
#include <cstdio>
extern "C" int destructorCount;
struct T {
  int n;
  T(int n) : n(n) {
    printf("    T(%d)\n", n);
  }
  T(const T &o) : n(o.n) {
    printf("    T(const T&) copy of %d\n", o.n);
  }
  ~T() {
    printf("    ~T(%d)\n", n);
    n = -1;
    ++destructorCount;
  }
};
)cpp";

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode",
      llvm::cl::desc(
          "Demonstration: temp, lvalue, copies, bug, bug-destroy, safe"),
      llvm::cl::cat(Options), llvm::cl::init("temp"));
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
  if (llvm::Error Error = I->ParseAndExecute(Prelude)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  if (Mode == "temp") {
    (llvm::outs() << ("evaluate T{7}:") << "\n");
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{7}
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    const char *KindName = "?";
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
    llvm::outs() << "kind=" << KindName
                 << " manuallyAlloc=" << V.isManuallyAlloc()
                 << " type=" << V.getType().getAsString()
                 << " n=" << *(int *)V.getPtr() << "\n";
    (llvm::outs() << ("V goes out of scope:") << "\n");
  } else if (Mode == "lvalue") {
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T t(1);
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    (llvm::outs() << ("evaluate the lvalue t:") << "\n");
    {
      Value V;
      if (llvm::Error Error = I->ParseAndExecute(R"cpp(
t
)cpp",
                                                 &V)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
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
      llvm::outs() << "kind=" << KindName2
                   << " manuallyAlloc=" << V.isManuallyAlloc() << " type="
                   << V.getType().getAsString(
                          V.getASTContext().getPrintingPolicy())
                   << "\n";
      (llvm::outs() << ("V goes out of scope:") << "\n");
    }
    (llvm::outs() << ("V is gone; t still lives in the interpreter:") << "\n");
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
t.n
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
  } else if (Mode == "copies") {
    (llvm::outs() << ("evaluate T{8}:") << "\n");
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{8}
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    {
      // Copying a Value retains the same object storage; it does not
      // copy-construct T.
      Value C1 = V;
      Value C2 = C1;
      llvm::outs() << "three owners, same storage: "
                   << (C1.getPtr() == V.getPtr() && C2.getPtr() == V.getPtr())
                   << ", dtor runs so far: " << destructorCount << "\n";
      (llvm::outs() << ("C1 and C2 die:") << "\n");
    }
    llvm::outs() << "dtor runs so far: " << destructorCount << "\n";
    (llvm::outs() << ("V.clear():") << "\n");
    // Release this Value's share; the last owner runs the captured object's
    // destructor.
    V.clear();
    llvm::outs() << "dtor runs so far: " << destructorCount
                 << ", V valid: " << V.isValid() << "\n";
    (llvm::outs() << ("move-assignment is safe:") << "\n");
    Value S1, S2;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{9}
)cpp",
                                               &S1)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    // Move assignment transfers an existing share safely; move construction is
    // broken in LLVM 22.1.8.
    S2 = std::move(S1);
    llvm::outs() << "dtor runs so far: " << destructorCount
                 << ", S1 valid: " << S1.isValid()
                 << ", S2 n=" << *(int *)S2.getPtr() << "\n";
    (llvm::outs() << ("S2 goes out of scope:") << "\n");
  } else if (Mode == "bug") {
    Value A1;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{9}
)cpp",
                                               &A1)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    int *Payload = (int *)A1.getPtr();
    llvm::outs() << "A1 owns n=" << *Payload
                 << ", dtor runs: " << destructorCount << "\n";
    (llvm::outs() << ("Value *B = new Value(std::move(A1)):") << "\n");
    // The heap Value is never deleted on purpose: its destructor would release
    // storage that is already gone.
    // Deliberately exercise LLVM 22.1.8's broken move constructor, leaving a
    // dangling owner.
    // This failure demonstration is not a pattern for retaining a result.
    Value *B = new Value(std::move(A1));
    llvm::outs() << "dtor runs: " << destructorCount
                 << ", A1 valid: " << A1.isValid()
                 << ", B valid: " << B->isValid()
                 << ", B manuallyAlloc: " << B->isManuallyAlloc() << "\n";
    // Undefined behaviour, shown on purpose: the block was freed (after the
    // destructor stored -1 into it), so what the allocator left there is not 9.
    llvm::outs() << "B still points at "
                 << (B->getPtr() == Payload ? "the same payload"
                                            : "something else")
                 << ", and its n is "
                 << (*Payload == 9 ? "still 9" : "no longer 9") << "\n";
  } else if (Mode == "bug-destroy") {
    // The same bug, but B is allowed to die: its destructor Release()s the
    // storage a second time.
    Value A1;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{9}
)cpp",
                                               &A1)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    {
      Value B = std::move(A1);
      (llvm::outs() << ("B goes out of scope:") << "\n");
    }
    (llvm::outs() << ("survived") << "\n");
  } else if (Mode == "safe") {
    (llvm::outs() << ("copy construction instead of move construction:")
                  << "\n");
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
T{9}
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    Value B = V; // retains: both own the object
    llvm::outs() << "dtor runs: " << destructorCount
                 << ", B n=" << *(int *)B.getPtr() << "\n";
    (llvm::outs() << ("both go out of scope:") << "\n");
  }
  return 0;
}
