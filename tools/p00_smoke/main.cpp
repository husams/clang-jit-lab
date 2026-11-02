// p00_smoke -- prove the toolchain works: build, link, JIT, headers, values.
//
// Usage: build/bin/p00_smoke
//
// Touches each piece the later parts rely on once: the interpreter is created
// with the platform flags, the C++ standard library is found, JIT'd code runs
// and prints, a clang::Value comes back, and a JIT'd function is called from
// the host through its symbol address.
#include "repllab.h"

using namespace clang;

int main() {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p00_smoke: ");

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter({"-std=c++20"}));
  llvm::outs() << "ok  interpreter created\n";

  ExitOnErr(I->ParseAndExecute("#include <vector>\n#include <cstdio>\n"));
  llvm::outs() << "ok  <vector> and <cstdio> found\n";

  ExitOnErr(I->ParseAndExecute("std::vector<int> v{1, 2, 3}; std::printf(\"ok  JIT printf, v.size() = %zu\\n\", v.size());"));

  Value V;
  ExitOnErr(I->ParseAndExecute("v[0] + v[1] + v[2]", &V));
  llvm::outs() << "ok  Value: " << repllab::valueString(V);

  ExitOnErr(I->ParseAndExecute("extern \"C\" int smoke_twice(int x) { return 2 * x; }"));
  auto Addr = ExitOnErr(I->getSymbolAddress("smoke_twice"));
  llvm::outs() << "ok  host call: smoke_twice(21) = " << Addr.toPtr<int (*)(int)>()(21) << "\n";
  return 0;
}
