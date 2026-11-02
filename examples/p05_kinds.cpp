#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace clang;

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: table, natural, wrong, ptr"),
      llvm::cl::cat(Options), llvm::cl::init("table"));
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

  if (Mode == "table") {
    {

      llvm::outs() << "kind  name           C++ type            sizeof\n";
#define X(type, name)                                                          \
  llvm::outs() << llvm::format("%-5d %-14s %-19s %zu\n", (int)Value::K_##name, \
                               "K_" #name, #type, sizeof(type));
      REPL_BUILTIN_TYPES
#undef X
      llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_Void,
                                   "K_Void");
      llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_PtrOrObj,
                                   "K_PtrOrObj");
      llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_Unspecified,
                                   "K_Unspecified");
      // The storage union is as wide as 8 long doubles; the rest of a Value is
      // a few words.
      llvm::outs() << "sizeof(clang::Value) = " << sizeof(Value)
                   << ", of which the storage union is "
                   << sizeof(long double) * 8 << " bytes\n";
    }
    return 0;
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
  // Compile this input, then submit its module and run its initializers.
  if (llvm::Error Error = I->ParseAndExecute(
          R"cpp(
enum Color : unsigned char { Red, Green = 5 };
enum Plain { A, B };
enum class Cls : short { X = -2 };
int answer = 42;
double pi = 2.9;
float f = 1.5f;
int *pa = &answer;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  if (Mode == "natural") {
    for (const char *Code : {R"cpp(
answer
)cpp",
                             R"cpp(
pi
)cpp",
                             R"cpp(
f
)cpp",
                             R"cpp(
'A'
)cpp",
                             R"cpp(
(bool)3
)cpp",
                             R"cpp(
(unsigned char)200
)cpp",
                             R"cpp(
Green
)cpp",
                             R"cpp(
B
)cpp",
                             R"cpp(
Cls::X
)cpp",
                             R"cpp(
10UL
)cpp",
                             R"cpp(
2.5L
)cpp"}) {
      Value V;
      if (llvm::Error Error = I->ParseAndExecute(Code, &V)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      std::string Ty =
          V.getType().getAsString(V.getASTContext().getPrintingPolicy());
      const char *KindName = "?";
      // The kind selects the active storage member; the macro enumerates
      // Clang's builtin kinds.
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
      std::string NaturalValue;

      llvm::raw_string_ostream ValueOutput(NaturalValue);
      switch (V.getKind()) {
#define X(type, name)                                                          \
  case Value::K_##name:                                                        \
    ValueOutput << "get" #name "() = ";                                        \
    if constexpr (std::is_integral_v<type> && std::is_unsigned_v<type>) {      \
      ValueOutput << static_cast<unsigned long long>(V.get##name());           \
    } else if constexpr (std::is_integral_v<type>) {                           \
      ValueOutput << static_cast<long long>(V.get##name());                    \
    } else {                                                                   \
      ValueOutput << llvm::format("%g", static_cast<double>(V.get##name()));   \
    }                                                                          \
    break;
        REPL_BUILTIN_TYPES
#undef X
      case Value::K_Void:
        ValueOutput << "(void)";
        break;
      case Value::K_PtrOrObj:
        ValueOutput << "getPtr() is an address";
        break;
      case Value::K_Unspecified:
        ValueOutput << "(no value)";
        break;
      }

      llvm::outs() << llvm::format("%-20s %-15s %-13s ", Code, Ty.c_str(),
                                   KindName)
                   << NaturalValue << "\n";
    }
  } else if (Mode == "wrong") {
    // Reading a union member that does not match the kind reinterprets the
    // bytes.
    Value D, F, P, N;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
1.0
)cpp",
                                               &D)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
f
)cpp",
                                               &F)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
pi
)cpp",
                                               &P)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
answer
)cpp",
                                               &N)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "double 1.0   getDouble()          = " << D.getDouble()
                 << "\n";
    llvm::outs() << "double 1.0   getLong()            = " << D.getLong()
                 << "   (bit pattern)\n";
    // convertTo selects the stored kind and converts the number; a getter only
    // reads storage.
    llvm::outs() << "double 1.0   convertTo<long>()    = "
                 << D.convertTo<long>() << "   (a conversion)\n";
    llvm::outs() << "float 1.5f   getInt()             = " << F.getInt()
                 << "   (bit pattern)\n";
    llvm::outs() << "float 1.5f   convertTo<int>()     = " << F.convertTo<int>()
                 << "\n";
    llvm::outs() << "double 2.9   convertTo<int>()     = " << P.convertTo<int>()
                 << "   (truncates)\n";
    llvm::outs() << "int 42       convertTo<double>()  = "
                 << N.convertTo<double>() << "\n";
    llvm::outs() << "int 42       convertTo<bool>()    = "
                 << N.convertTo<bool>() << "\n";
  } else if (Mode == "ptr") {
    Value PV, IV, SV, OV;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
pa
)cpp",
                                               &PV)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
answer
)cpp",
                                               &IV)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
"hello"
)cpp",
                                               &SV)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
Color {
  Green
}
)cpp",
                                               &OV)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    // A pointer-kind Value exposes its address without taking ownership of the
    // pointee.
    int *P = PV.convertTo<int *>();
    const char *KindName2 = "?";
    switch (PV.getKind()) {
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
    llvm::outs() << "pa kind                      = " << KindName2 << "\n";
    llvm::outs() << "*pa read by the host         = " << *P << "\n";
    *P = 43;
    llvm::outs() << "after the host wrote 43      = ";
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
answer
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "convertTo<int *>() on an int = "
                 << (void *)IV.convertTo<int *>() << "\n";
    llvm::outs() << "convertTo<long>() on a ptr   = same as getPtr(): "
                 << (PV.convertTo<long>() == (long)PV.getPtr()) << "\n";
    const char *KindName3 = "?";
    switch (SV.getKind()) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    KindName3 = "K_" #name;                                                    \
    break;
      REPL_BUILTIN_TYPES
#undef X
    case clang::Value::K_Void:
      KindName3 = "K_Void";
      break;
    case clang::Value::K_PtrOrObj:
      KindName3 = "K_PtrOrObj";
      break;
    case clang::Value::K_Unspecified:
      KindName3 = "K_Unspecified";
      break;
    }
    llvm::outs() << "\"hello\" kind, owned          = " << KindName3 << ", "
                 << SV.isManuallyAlloc() << "\n";
    llvm::outs() << "convertTo<const char *>()    = "
                 << SV.convertTo<const char *>() << "\n";
    const char *KindName4 = "?";
    switch (OV.getKind()) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    KindName4 = "K_" #name;                                                    \
    break;
      REPL_BUILTIN_TYPES
#undef X
    case clang::Value::K_Void:
      KindName4 = "K_Void";
      break;
    case clang::Value::K_PtrOrObj:
      KindName4 = "K_PtrOrObj";
      break;
    case clang::Value::K_Unspecified:
      KindName4 = "K_Unspecified";
      break;
    }
    llvm::outs() << "Color{Green} (a temporary) = " << KindName4 << "\n";
  }
  return 0;
}
