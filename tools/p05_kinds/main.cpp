// p05_kinds -- the builtin kinds of clang::Value, the typed accessors, and
// convertTo<T>() (Section 5.2).
//
// Usage: build/bin/p05_kinds [table|natural|wrong|ptr]
#include "../p05_anatomy/kinds.h"

#include "clang/AST/ASTContext.h"

using namespace clang;

// The kind table, generated from the same X-macro that Value.h uses.
static void printKindTable() {
  llvm::outs() << "kind  name           C++ type            sizeof\n";
#define X(type, name)                                                                                              \
  llvm::outs() << llvm::format("%-5d %-14s %-19s %zu\n", (int)Value::K_##name, "K_" #name, #type, sizeof(type));
  REPL_BUILTIN_TYPES
#undef X
  llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_Void, "K_Void");
  llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_PtrOrObj, "K_PtrOrObj");
  llvm::outs() << llvm::format("%-5d %-14s\n", (int)Value::K_Unspecified, "K_Unspecified");
  // The storage union is as wide as 8 long doubles; the rest of a Value is a few words.
  llvm::outs() << "sizeof(clang::Value) = " << sizeof(Value) << ", of which the storage union is "
               << sizeof(long double) * 8 << " bytes\n";
}

// raw_ostream has no long double, and prints char as a character: normalise both.
template <typename T> static void put(llvm::raw_ostream &OS, T V) {
  if constexpr (std::is_floating_point_v<T>)
    OS << llvm::format("%g", (double)V);
  else
    OS << +V;
}

// "The natural read": switch on getKind() and call the matching typed getter.
// This is the code Value.h's own as<T>() has inside, written out with the macro.
static std::string natural(const Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  switch (V.getKind()) {
#define X(type, name)                                                                                              \
  case Value::K_##name:                                                                                            \
    OS << "get" #name "() = ", put(OS, V.get##name());                                                                  \
    break;
    REPL_BUILTIN_TYPES
#undef X
  case Value::K_Void:
    OS << "(void)";
    break;
  case Value::K_PtrOrObj:
    OS << "getPtr() is an address";
    break;
  case Value::K_Unspecified:
    OS << "(no value)";
    break;
  }
  return S;
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_kinds: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::string Mode = A.Positional.empty() ? "table" : A.Positional[0];

  if (Mode == "table") {
    printKindTable();
    return 0;
  }

  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  // Out-parameter, not a returned Value: see Section 5.3 (broken move constructor).
  auto eval = [&](llvm::StringRef Code, Value &Out) { return repllab::run(*I, Code, &Out); };
  repllab::run(*I, "enum Color : unsigned char { Red, Green = 5 }; enum Plain { A, B };"
                   "enum class Cls : short { X = -2 };"
                   "int answer = 42; double pi = 2.9; float f = 1.5f; int *pa = &answer;");

  if (Mode == "natural") {
    for (const char *Code : {"answer", "pi", "f", "'A'", "(bool)3", "(unsigned char)200", "Green", "B", "Cls::X",
                             "10UL", "2.5L"}) {
      Value V;
      eval(Code, V);
      std::string Ty = V.getType().getAsString(V.getASTContext().getPrintingPolicy());
      llvm::outs() << llvm::format("%-20s %-15s %-13s ", Code, Ty.c_str(), p05::kindName(V.getKind())) << natural(V)
                   << "\n";
    }
  } else if (Mode == "wrong") {
    // Reading a union member that does not match the kind reinterprets the bytes.
    Value D, F, P, N;
    eval("1.0", D);
    eval("f", F);
    eval("pi", P);
    eval("answer", N);
    llvm::outs() << "double 1.0   getDouble()          = " << D.getDouble() << "\n";
    llvm::outs() << "double 1.0   getLong()            = " << D.getLong() << "   (bit pattern)\n";
    llvm::outs() << "double 1.0   convertTo<long>()    = " << D.convertTo<long>() << "   (a conversion)\n";
    llvm::outs() << "float 1.5f   getInt()             = " << F.getInt() << "   (bit pattern)\n";
    llvm::outs() << "float 1.5f   convertTo<int>()     = " << F.convertTo<int>() << "\n";
    llvm::outs() << "double 2.9   convertTo<int>()     = " << P.convertTo<int>() << "   (truncates)\n";
    llvm::outs() << "int 42       convertTo<double>()  = " << N.convertTo<double>() << "\n";
    llvm::outs() << "int 42       convertTo<bool>()    = " << N.convertTo<bool>() << "\n";
  } else if (Mode == "ptr") {
    Value PV, IV, SV, OV;
    eval("pa", PV);
    eval("answer", IV);
    eval("\"hello\"", SV);
    eval("Color{Green}", OV);
    int *P = PV.convertTo<int *>();
    llvm::outs() << "pa kind                      = " << p05::kindName(PV.getKind()) << "\n";
    llvm::outs() << "*pa read by the host         = " << *P << "\n";
    *P = 43;
    llvm::outs() << "after the host wrote 43      = ";
    repllab::run(*I, "answer");
    llvm::outs() << "convertTo<int *>() on an int = " << (void *)IV.convertTo<int *>() << "\n";
    llvm::outs() << "convertTo<long>() on a ptr   = same as getPtr(): " << (PV.convertTo<long>() == (long)PV.getPtr()) << "\n";
    llvm::outs() << "\"hello\" kind, owned          = " << p05::kindName(SV.getKind()) << ", " << SV.isManuallyAlloc() << "\n";
    llvm::outs() << "convertTo<const char *>()    = " << SV.convertTo<const char *>() << "\n";
    llvm::outs() << "Color{Green} (a temporary) = " << p05::kindName(OV.getKind()) << "\n";
  }
  return 0;
}
