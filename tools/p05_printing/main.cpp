// p05_printing -- the Value printing API, with and without a Value, and a
// pretty printer built by asking the interpreter questions (Section 5.4).
//
// Usage: build/bin/p05_printing apis|capture|pretty setup.repl [-- compiler flags]
#include "../p05_anatomy/kinds.h"

#include "clang/AST/DeclTemplate.h"

using namespace clang;

static std::string capture(void (Value::*Fn)(llvm::raw_ostream &) const, const Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  (V.*Fn)(OS);
  return repllab::maskAddrs(S);
}

// ---- pretty printing ------------------------------------------------------

// Evaluate `Code` into Out. Out is a parameter, not a return value: a Value
// returned by value is only safe through copy elision (its move constructor is
// broken in 22.1.8, Section 5.3).
static bool eval(Interpreter &I, const std::string &Code, Value &Out) { return repllab::run(I, Code, &Out); }

// The address of the object an lvalue expression names, spelled as a cast to
// `const T *` that can be pasted into source.
static std::string ptrExpr(const Value &V, const std::string &T) {
  return "((const " + T + " *)" + std::to_string((uintptr_t)V.getPtr()) + "UL)";
}

// std::string -> "text"; std::vector<T> -> [a, b, c] (elements pretty-printed
// recursively); everything else: the interpreter's own printing.
static std::string prettyValue(Interpreter &I, const Value &V) {
  if (!V.isValid())
    return "<error>";
  // Canonical: `std::string` is a typedef, the pattern below wants what it names.
  QualType QT = V.getType().getNonReferenceType().getCanonicalType().getUnqualifiedType();
  std::string T = QT.getAsString(V.getASTContext().getPrintingPolicy());
  bool IsRef = V.getType()->isReferenceType();
  // Recognise the class by its declaration, not by the spelling of its type.
  const CXXRecordDecl *RD = QT->getAsCXXRecordDecl();
  bool InStd = RD && RD->isInStdNamespace();

  if (IsRef && InStd && RD->getName() == "basic_string") {
    Value S;
    eval(I, ptrExpr(V, T) + "->c_str()", S);
    return std::string("\"") + S.convertTo<const char *>() + "\"";
  }
  if (IsRef && InStd && RD->getName() == "vector") {
    // The element type comes from the AST, not from string surgery.
    const auto *Spec = llvm::cast<ClassTemplateSpecializationDecl>(RD);
    std::string Elt = Spec->getTemplateArgs()[0].getAsType().getAsString(V.getASTContext().getPrintingPolicy());
    Value NV;
    eval(I, ptrExpr(V, T) + "->size()", NV);
    size_t N = NV.convertTo<size_t>();
    std::string Out = "[";
    for (size_t K = 0; K < N; ++K) {
      Value E;
      eval(I, "(*" + ptrExpr(V, T) + ")[" + std::to_string(K) + "]", E);
      Out += (K ? ", " : "") + prettyValue(I, E);
    }
    return Out + "]";
  }
  // Fallback: the built-in printer, minus the "(type) " prefix.
  return capture(&Value::printData, V);
}

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_printing: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 2) {
    llvm::errs() << "usage: p05_printing apis|capture|pretty setup.repl\n";
    return 2;
  }
  std::string Mode = A.Positional[0];
  // nodiscard on size() and operator[] would warn on every probe we evaluate.
  A.Compiler.insert(A.Compiler.begin(), "-Wno-unused-result");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string &Code : repllab::replInputs(repllab::readFile(A.Positional[1])))
    ExitOnErr(I->ParseAndExecute(Code));

  if (Mode == "apis") {
    for (const char *Expr : {"f", "d", "cs", "ch", "(Color)5", "(Color)3", "arr", "grid", "ip", "fp", "twice", "p",
                             "name"}) {
      Value V;
      eval(*I, Expr, V);
      llvm::outs() << llvm::format("%-9s", Expr) << "type: " << capture(&Value::printType, V) << "\n"
                   << llvm::format("%-9s", "") << "data: " << capture(&Value::printData, V) << "\n";
    }
  } else if (Mode == "capture") {
    llvm::outs() << "without a Value, the interpreter prints:\n";
    repllab::run(*I, "nums.size()");
    llvm::outs() << "with a Value, nothing is printed:\n";
    Value V;
    eval(*I, "nums.size()", V);
    llvm::outs() << "then print(), dump() and printData() are ours to call:\n";
    V.print(llvm::outs());
    V.dump();
    V.printData(llvm::outs());
    llvm::outs() << "\n";
    llvm::outs() << "a declaration prints nothing either way: ";
    Value W;
    repllab::run(*I, "int unused = 0;", &W);
    llvm::outs() << "isValid=" << W.isValid() << "\n";
  } else if (Mode == "pretty") {
    for (const char *Expr : {"name", "nums", "words", "p", "name.size()", "arr"}) {
      Value V;
      eval(*I, Expr, V);
      llvm::outs() << llvm::format("%-12s", Expr) << "default: " << capture(&Value::print, V);
      llvm::outs() << llvm::format("%-12s", "") << "pretty:  " << prettyValue(*I, V) << "\n";
    }
  }
  return 0;
}
