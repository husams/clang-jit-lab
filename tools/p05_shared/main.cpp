// p05_shared -- share variables and functions between the tool and the code it
// interprets, through the process's own symbols (Section 5.5).
//
// Usage: build/bin/p05_shared extern|cpp|static|jit2host
#include "../p05_anatomy/kinds.h"

using namespace clang;

// Visible to the JIT: external linkage, defined in the executable.
extern "C" int host_counter = 10;
extern "C" const char *host_name = "the host";
extern "C" int host_twice(int X) { return 2 * X; }

// C++ linkage: the symbol is mangled (__Z12host_cpp_addii on this platform).
int host_cpp_add(int A, int B) { return A + B; }
namespace host { int ns_scale(int X) { return 100 * X; } }

// Internal linkage: not an exported symbol, so the JIT cannot find it.
static int host_hidden(int X) { return X + 1; }

// Called FROM the interpreter; prints so the order of events is visible.
extern "C" void host_log(const char *Msg) { llvm::outs() << "  [host_log] " << Msg << "\n"; }

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_shared: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::string Mode = A.Positional.empty() ? "extern" : A.Positional[0];
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  auto eval = [&](const char *Code) { repllab::run(*I, Code); };

  if (Mode == "extern") {
    eval("extern \"C\" int host_counter;");
    eval("extern \"C\" int host_twice(int);");
    eval("extern \"C\" const char *host_name;");
    eval("extern \"C\" void host_log(const char *);");
    llvm::outs() << "host sees host_counter = " << host_counter << "\n";
    eval("host_counter");
    eval("host_counter += 5;");
    llvm::outs() << "host sees host_counter = " << host_counter << " after the interpreter added 5\n";
    host_counter = 100;
    eval("host_counter");
    eval("host_twice(21)");
    eval("host_name");
    eval("host_log(\"called from interpreted code\");");
    // Where does the interpreter's `host_counter` live? In the host.
    Value V;
    repllab::run(*I, "&host_counter", &V);
    llvm::outs() << "&host_counter in the JIT == &host_counter in C++: " << (V.convertTo<int *>() == &host_counter) << "\n";
  } else if (Mode == "cpp") {
    eval("int host_cpp_add(int, int);");
    eval("namespace host { int ns_scale(int); }");
    eval("host_cpp_add(2, 3)");
    eval("host::ns_scale(4)");
    // The interpreter's own function, found by IR name (the mangled name):
    eval("int jit_sub(int a, int b) { return a - b; }");
    auto Addr = ExitOnErr(I->getSymbolAddress("_Z7jit_subii"));
    auto *Sub = Addr.toPtr<int (*)(int, int)>();
    llvm::outs() << "host calls jit_sub(9, 4) = " << Sub(9, 4) << "\n";
  } else if (Mode == "static") {
    llvm::outs() << "host_hidden(1) in C++ = " << host_hidden(1) << "\n";
    eval("int host_hidden(int);");
    eval("host_hidden(1)");
  } else if (Mode == "jit2host") {
    eval("int jit_total = 0;");
    eval("void jit_add(int n) { jit_total += n; }");
    auto Add = ExitOnErr(I->getSymbolAddress("_Z7jit_addi")).toPtr<void (*)(int)>();
    auto Total = ExitOnErr(I->getSymbolAddress("jit_total")).toPtr<int *>();
    for (int N : {1, 2, 3})
      Add(N);
    llvm::outs() << "host called jit_add 3 times; jit_total read from C++ = " << *Total << "\n";
    *Total = 1000;
    eval("jit_total");
  }
  return 0;
}
