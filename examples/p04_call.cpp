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
