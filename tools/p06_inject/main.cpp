// p06_inject -- the interpreter writes code for itself (Section 6.2).
//
// A custom ASTConsumer notices every class the user defines. When the input
// has finished, the tool reads the class's fields from the AST, generates an
// `operator<<(std::ostream &, const T &)` that prints every field by name, and
// feeds it back with ParseAndExecute. The next input can then stream the type.
//
//   --unsafe   inject from inside the consumer callback instead of after the
//              input (the mistake the section explains)
//
// Usage: build/bin/p06_inject [--unsafe] inputs.repl [-- compiler flags]
#include "repllab.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"

using namespace clang;

namespace {

// Builds the C++ source of operator<< for a record, or "" if we cannot do it.
static std::string generatePrinter(const CXXRecordDecl *R, std::string &WhySkipped) {
  const ASTContext &Ctx = R->getASTContext();
  std::string Name = R->getQualifiedNameAsString();
  std::string Body = "os << \"" + Name + "{\"";
  bool First = true;
  for (const FieldDecl *F : R->fields()) {
    if (F->getAccess() != AS_public) {
      WhySkipped = "field '" + F->getNameAsString() + "' is not public";
      return "";
    }
    QualType T = F->getType().getCanonicalType();
    std::string Field = "v." + F->getNameAsString();
    std::string Value;
    if (T->isEnumeralType())
      Value = "static_cast<long long>(" + Field + ")";
    else if (T->isBooleanType())
      Value = "(" + Field + " ? \"true\" : \"false\")";
    else if (Ctx.getAsConstantArrayType(T)) {
      // arrays print as {a, b, c}
      Value = "[&] { std::ostringstream t; const char *sep = \"\"; t << \"{\"; for (const auto &e : " + Field +
              ") { t << sep << e; sep = \", \"; } t << \"}\"; return t.str(); }()";
    } else
      Value = Field;
    Body += std::string(" << \"") + (First ? "" : ", ") + F->getNameAsString() + "=\" << " + Value;
    First = false;
  }
  Body += " << \"}\"";
  return "std::ostream &operator<<(std::ostream &os, const " + Name + " &v) { return " + Body + "; }";
}

class RecordWatcher : public ASTConsumer {
public:
  Interpreter *Interp = nullptr; // for --unsafe only
  bool Unsafe = false;
  std::vector<const CXXRecordDecl *> Pending;

  // Called by the parser when a tag (struct/class/union/enum) definition is
  // complete. We are in the middle of parsing the user's input.
  void HandleTagDeclDefinition(TagDecl *D) override {
    auto *R = llvm::dyn_cast<CXXRecordDecl>(D);
    if (!R || R->isImplicit() || R->isLambda() || R->isUnion() || R->getDescribedClassTemplate() ||
        R->getDeclContext()->isRecord() || R->getDeclContext()->isFunctionOrMethod() ||
        R->getName().empty())
      return;
    // Headers define classes too; only the ones typed at the prompt (memory
    // buffers named input_line_<N>) are interesting.
    const SourceManager &SM = R->getASTContext().getSourceManager();
    if (!llvm::StringRef(SM.getPresumedLoc(SM.getExpansionLoc(R->getLocation())).getFilename())
             .starts_with("input_line_"))
      return;
    llvm::outs() << "  [watch] saw class " << R->getQualifiedNameAsString() << "\n";
    if (!Unsafe) {
      Pending.push_back(R);
      return;
    }
    std::string Why, Src = generatePrinter(R, Why);
    llvm::outs() << "  [inject] (from inside the callback)\n";
    repllab::run(*Interp, Src); // re-enters the parser while it is parsing
  }
};

class WatchingInterpreter : public Interpreter {
public:
  WatchingInterpreter(std::unique_ptr<CompilerInstance> CI, llvm::Error &Err, std::unique_ptr<ASTConsumer> C)
      : Interpreter(std::move(CI), Err, nullptr, std::move(C)) {}
};

} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_inject: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  bool Unsafe = false;
  std::vector<std::string> Files;
  for (const std::string &P : A.Positional) {
    if (P == "--unsafe")
      Unsafe = true;
    else
      Files.push_back(P);
  }

  auto W = std::make_unique<RecordWatcher>();
  RecordWatcher *Watcher = W.get();
  Watcher->Unsafe = Unsafe;
  auto CI = ExitOnErr(repllab::makeCompiler(A.Compiler));
  llvm::Error Err = llvm::Error::success();
  WatchingInterpreter I(std::move(CI), Err, std::move(W));
  ExitOnErr(std::move(Err));
  Watcher->Interp = &I;

  repllab::run(I, "#include <iostream>");
  repllab::run(I, "#include <sstream>");
  repllab::run(I, "#include <string>");

  for (const std::string &File : Files) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      llvm::outs() << "input: " << Code << "\n";
      if (!repllab::run(I, Code)) {
        Watcher->Pending.clear(); // the input was rolled back: its classes do not exist
        continue;
      }

      // The input is finished: nothing is parsing, so injecting is safe now.
      std::vector<const CXXRecordDecl *> Todo;
      Todo.swap(Watcher->Pending);
      for (const CXXRecordDecl *R : Todo) {
        std::string Why, Src = generatePrinter(R, Why);
        if (Src.empty()) {
          llvm::outs() << "  [inject] skipped " << R->getQualifiedNameAsString() << ": " << Why << "\n";
          continue;
        }
        llvm::outs() << "  [inject] " << Src << "\n";
        repllab::run(I, Src);
      }
    }
  }
  return 0;
}
