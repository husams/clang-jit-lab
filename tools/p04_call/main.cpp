// p04_call -- call JIT-compiled code from the host (Section 4.9): look a
// symbol up by IR name, by GlobalDecl, by linker name; read and write a JIT
// global; call a JIT function in a loop.
//
// Usage: build/bin/p04_call
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Interpreter/Interpreter.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Error.h"

#include "repllab.h"

using namespace clang;

int main() {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p04_call: ");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter());

  // --- 1. by IR name: extern "C" has no mangling, C++ does ---
  llvm::outs() << "== getSymbolAddress(StringRef): the name as written in the IR\n";
  ExitOnErr(I->ParseAndExecute("extern \"C\" int c_add(int a, int b) { return a + b; }"));
  ExitOnErr(I->ParseAndExecute("int cxx_mul(int a, int b) { return a * b; }"));
  auto CAdd = ExitOnErr(I->getSymbolAddress("c_add"));
  auto CxxMul = ExitOnErr(I->getSymbolAddress("_Z7cxx_mulii"));
  llvm::outs() << "c_add(2, 3)          = " << CAdd.toPtr<int (*)(int, int)>()(2, 3) << "\n";
  llvm::outs() << "_Z7cxx_mulii(4, 5)   = " << CxxMul.toPtr<int (*)(int, int)>()(4, 5) << "\n";
  llvm::outs() << "lookup of \"cxx_mul\"  : " << llvm::toString(I->getSymbolAddress("cxx_mul").takeError()) << "\n";

  // --- 2. by GlobalDecl: no manual mangling ---
  llvm::outs() << "== getSymbolAddress(GlobalDecl): take the decl from the PTU\n";
  PartialTranslationUnit &PTU = ExitOnErr(I->Parse("double scale(double v) { return v * 1.5; }"));
  FunctionDecl *FD = nullptr;
  for (Decl *D : PTU.TUPart->decls())
    if (auto *F = llvm::dyn_cast<FunctionDecl>(D))
      FD = F;
  ExitOnErr(I->Execute(PTU));
  auto Scale = ExitOnErr(I->getSymbolAddress(GlobalDecl(FD)));
  llvm::outs() << FD->getQualifiedNameAsString() << "(2.0) = " << Scale.toPtr<double (*)(double)>()(2.0) << "\n";

  // The same name, computed by hand with a MangleContext.
  std::unique_ptr<MangleContext> MC(I->getASTContext().createMangleContext());
  std::string Mangled;
  llvm::raw_string_ostream OS(Mangled);
  MC->mangleName(GlobalDecl(FD), OS);
  llvm::outs() << "MangleContext says: " << Mangled << "\n";

  // --- 3. by linker name: Mach-O adds a leading underscore ---
  llvm::outs() << "== getSymbolAddressFromLinkerName\n";
  std::string LinkerName;
  // LLVM's DataLayout knows whether the target object format adds a prefix.
  llvm::DataLayout Layout(I->getASTContext().getTargetInfo().getDataLayoutString());
  if (char Prefix = Layout.getGlobalPrefix())
    LinkerName += Prefix;
  LinkerName += "_Z7cxx_mulii";
  auto ByLinker = ExitOnErr(I->getSymbolAddressFromLinkerName(LinkerName));
  llvm::outs() << "same address as the IR name: " << (ByLinker == CxxMul ? "yes" : "no") << "\n";
  llvm::outs() << "IR name as a linker name   : ";
  if (auto IRAsLinker = I->getSymbolAddressFromLinkerName("_Z7cxx_mulii"))
    llvm::outs() << "found (no extra target prefix)\n";
  else
    llvm::outs() << llvm::toString(IRAsLinker.takeError()) << "\n";

  // --- 4. data: read and write a JIT global ---
  llvm::outs() << "== a JIT global through its address\n";
  ExitOnErr(I->ParseAndExecute("int counter = 10;"));
  int *Counter = ExitOnErr(I->getSymbolAddress("counter")).toPtr<int *>();
  llvm::outs() << "host reads counter = " << *Counter << "\n";
  *Counter = 99;
  llvm::outs() << "host wrote 99, the JIT sees:\n";
  ExitOnErr(I->ParseAndExecute("counter"));
  ExitOnErr(I->ParseAndExecute("counter += 1;"));
  llvm::outs() << "the JIT added 1, the host sees counter = " << *Counter << "\n";

  // --- 5. call it many times: a plain function pointer from now on ---
  llvm::outs() << "== a host loop calling JIT code\n";
  auto Add = CAdd.toPtr<int (*)(int, int)>();
  long Sum = 0;
  for (int K = 1; K <= 100000; ++K)
    Sum += Add(K, 1);
  llvm::outs() << "100000 calls, final value " << Sum << "\n";
  return 0;
}
