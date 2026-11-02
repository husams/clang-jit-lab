// p05_inject -- give interpreted code a pointer to host data, and read
// interpreter-owned data from the host (Section 5.6).
//
// Usage: build/bin/p05_inject config|callback|array|reverse
#include "../p05_anatomy/kinds.h"
#include "../../manifests/p05_config.h" // the very file the interpreter includes below

#include <cstring>
#include <numeric>

using namespace clang;

extern "C" void host_report(const char *What, int Value) {
  llvm::outs() << "  [host_report] " << What << " = " << Value << "\n";
}

// A pointer, spelled as a C++ expression the interpreter can parse.
static std::string hex(const void *P) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << llvm::format("0x%llxUL", (unsigned long long)(uintptr_t)P);
  return S;
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_inject: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::string Mode = A.Positional.empty() ? "config" : A.Positional[0];
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  // Show the input (addresses masked), then run it.
  auto eval = [&](const std::string &Code, Value *V = nullptr) {
    llvm::outs() << "> " << repllab::maskAddrs(Code) << "\n";
    return repllab::run(*I, Code, V);
  };
  // An absolute path, so the tool works from any directory.
  ExitOnErr(I->ParseAndExecute("#include \"" REPLLAB_LAB_ROOT "/manifests/p05_config.h\""));

  if (Mode == "config") {
    Config Host = {1, 1.5, "host", {10, 20, 30, 40}};
    eval("auto *cfg = (Config *)" + hex(&Host) + ";");
    eval("cfg->verbosity");
    eval("cfg->name");
    eval("cfg->limits[2]");
    eval("cfg->scale *= 2; cfg->verbosity = 7; cfg->limits[3] = -1;");
    eval("__builtin_strcpy(cfg->name, \"jit wrote\");");
    llvm::outs() << "host now sees: verbosity=" << Host.verbosity << " scale=" << llvm::format("%g", Host.scale) << " name=" << Host.name
                 << " limits[3]=" << Host.limits[3] << "\n";
    eval("sizeof(Config)");
    llvm::outs() << "host sizeof(Config) = " << sizeof(Config) << "\n";
  } else if (Mode == "callback") {
    ReportFn Fn = host_report;
    eval("auto report = (ReportFn)" + hex((const void *)Fn) + ";");
    eval("report(\"answer\", 42);");
    eval("for (int i = 1; i <= 3; ++i) report(\"square\", i * i);");
  } else if (Mode == "array") {
    int Data[8] = {3, 1, 4, 1, 5, 9, 2, 6};
    eval("int *data = (int *)" + hex(Data) + "; const int n = 8;");
    eval("int sum = 0; for (int i = 0; i < n; ++i) sum += data[i];");
    eval("sum");
    llvm::outs() << "host computes the same sum: " << std::accumulate(Data, Data + 8, 0) << "\n";
    eval("for (int i = 0; i < n; ++i) data[i] *= data[i];");
    llvm::outs() << "host array after the interpreter squared it:";
    for (int X : Data)
      llvm::outs() << " " << X;
    llvm::outs() << "\n";
  } else if (Mode == "reverse") {
    eval("Config jit_cfg = {3, 0.25, \"made in jit\", {1, 2, 3, 4}};");
    Value V;
    eval("jit_cfg", &V); // an lvalue: V refers to the interpreter's variable
    auto *C = static_cast<Config *>(V.getPtr());
    llvm::outs() << "host reads jit_cfg: verbosity=" << C->verbosity << " scale=" << llvm::format("%g", C->scale) << " name=" << C->name
                 << " limits[3]=" << C->limits[3] << "\n";
    C->verbosity = 99;
    eval("jit_cfg.verbosity");
    Value T;
    eval("Config{5, 0.5, \"temporary\", {}}", &T); // an rvalue: T owns a copy
    auto *Tmp = static_cast<Config *>(T.getPtr());
    llvm::outs() << "host reads the temporary: name=" << Tmp->name << " owned=" << T.isManuallyAlloc()
                 << " same address as jit_cfg: " << (Tmp == C) << "\n";
  }
  return 0;
}
