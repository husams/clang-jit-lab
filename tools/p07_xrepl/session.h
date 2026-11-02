// session.h -- the state behind xrepl: the interpreter, a record per input, the last value.
#ifndef XREPL_SESSION_H
#define XREPL_SESSION_H

#include "repllab.h"

#include "clang/AST/Decl.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace xrepl {

// One accepted input. Created after Parse succeeds (so it parallels the
// interpreter's PartialTranslationUnit list), removed again by %undo.
struct Record {
  unsigned Number = 0;     // 1-based position among the live inputs
  std::string Source;      // what the user typed
  std::string IR;          // module text captured BEFORE Execute moved the module away
  bool HasIR = false;
  struct Entry {
    clang::Decl *D;
    bool User; // spelled in the input itself, not pulled in by an #include
  };
  std::vector<Entry> Decls; // the top-level declarations of this input's TU part
};

struct Timing {
  double ParseMs = 0;
  double ExecMs = 0;
};

// What the front end says about an expression, without running it.
struct TypeInfo {
  clang::QualType Written;   // decltype((expr)), reference and all
  clang::QualType Object;    // the same without the reference
  enum Category { LValue, XValue, PRValue } Cat = PRValue;
  bool Sized = false;        // complete object type with a size
  uint64_t Size = 0, Align = 0;
};

class Session {
public:
  explicit Session(std::vector<std::string> CompilerArgs);
  ~Session();

  clang::Interpreter &interp() { return *I; }
  clang::ASTContext &ctx() { return I->getASTContext(); }
  const std::vector<std::string> &compilerArgs() const { return Args; }
  const std::deque<Record> &records() const { return Records; }
  bool deterministic() const { return Deterministic; }

  // A normal input: Parse, capture IR, Execute, print the value. Returns false after
  // reporting an error; the session stays usable.
  bool submit(llvm::StringRef Code, Timing *T = nullptr);

  // %undo N: undo in the interpreter, forget the records.
  bool undo(unsigned N);

  // Evaluate without leaving a trace: [run `Setup`,] Parse, Execute, hand the Value to `Use`, release it, Undo.
  bool withHidden(const std::string &Code, llvm::function_ref<void(clang::Value &)> Use,
                  const std::string &Setup = std::string());
  // The type of an expression: Parse of a type alias, read it, Undo. Nothing is executed.
  std::optional<TypeInfo> typeOf(const std::string &Expr);

  // The value of the last expression input (invalid if there is none).
  clang::Value &lastValue() { return Last; }
  void dropLastValue() { Last.clear(); }

  bool report(llvm::Error E);

private:
  void collectDecls(Record &R, clang::TranslationUnitDecl *TU);
  bool fetchValue(clang::Value &V);

  std::vector<std::string> Args;
  bool Deterministic = false;
  std::unique_ptr<clang::Interpreter> I;
  std::deque<Record> Records; // a deque never moves its elements
  // Declared after `I`, so it is destroyed first: an object Value runs its destructor in the JIT.
  clang::Value Last;
};

std::string maskIR(llvm::StringRef S);

} // namespace xrepl

#endif
