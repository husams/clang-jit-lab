#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

namespace {

class RecordWatcher : public ASTConsumer {
public:
  Interpreter *Interp = nullptr; // for --unsafe only
  bool Unsafe = false;
  std::vector<const CXXRecordDecl *> Pending;

  // Called by the parser when a tag (struct/class/union/enum) definition is
  // complete. We are in the middle of parsing the user's input.
  void HandleTagDeclDefinition(TagDecl *D) override {
    auto *R = llvm::dyn_cast<CXXRecordDecl>(D);
    if (!R || R->isImplicit() || R->isLambda() || R->isUnion() ||
        R->getDescribedClassTemplate() || R->getDeclContext()->isRecord() ||
        R->getDeclContext()->isFunctionOrMethod() || R->getName().empty()) {
      return;
    }
    // Headers define classes too; only the ones typed at the prompt (memory
    // buffers named input_line_<N>) are interesting.
    const SourceManager &SM = R->getASTContext().getSourceManager();
    if (!llvm::StringRef(SM.getPresumedLoc(SM.getExpansionLoc(R->getLocation()))
                             .getFilename())
             .starts_with("input_line_")) {
      return;
    }
    llvm::outs() << "  [watch] saw class " << R->getQualifiedNameAsString()
                 << "\n";
    if (!Unsafe) {
      // Record the declaration only; starting another parse inside this
      // callback would re-enter Clang.
      Pending.push_back(R);
      return;
    }
    std::string Why;
    std::string PrinterSource;
    bool Supported = true;

    const ASTContext &Ctx = R->getASTContext();
    std::string Name = R->getQualifiedNameAsString();
    std::string Body = R"cpp(
  os << ")cpp" + Name + R"cpp({"
)cpp";
    bool First = true;
    // Use field declarations and access control to generate code that can
    // legally read the record.
    for (const FieldDecl *F : R->fields()) {
      if (F->getAccess() != AS_public) {
        Why = "field '" + F->getNameAsString() + "' is not public";
        Supported = false;
        break;
      }
      QualType T = F->getType().getCanonicalType();
      std::string Field = R"cpp(v.)cpp" + F->getNameAsString();
      std::string Value;
      if (T->isEnumeralType()) {
        Value = R"cpp(static_cast<long long>()cpp" + Field + R"cpp())cpp";
      } else if (T->isBooleanType()) {
        Value = R"cpp(()cpp" + Field + R"cpp( ? "true" : "false"))cpp";
      } else if (Ctx.getAsConstantArrayType(T)) {
        Value = R"cpp(
[&] {
  std::ostringstream text;
  const char *separator = "";
  text << "{";
  for (const auto &element : )cpp" +
                Field + R"cpp() {
    text << separator << element;
    separator = ", ";
  }
  text << "}";
  return text.str();
}()
)cpp";
      } else {
        Value = Field;
      }
      Body += std::string(R"cpp(
      << ")cpp") +
              (First ? "" : ", ") + F->getNameAsString() + R"cpp(=" << )cpp" +
              Value;
      First = false;
    }
    Body += R"cpp(
      << "}";
  return os;
)cpp";
    if (Supported) {
      PrinterSource = R"cpp(
std::ostream &operator<<(std::ostream &os, const )cpp" +
                      Name + R"cpp( &v) {
)cpp" + Body + R"cpp(
}
)cpp";
    }

    std::string Src = PrinterSource;
    llvm::outs() << "  [inject] (from inside the callback)\n";
    if (llvm::Error Error = Interp->ParseAndExecute(Src)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    } // re-enters the parser while it is parsing
  }
};

class WatchingInterpreter : public Interpreter {
public:
  WatchingInterpreter(std::unique_ptr<CompilerInstance> CI, llvm::Error &Err,
                      std::unique_ptr<ASTConsumer> C)
      : Interpreter(std::move(CI), Err, nullptr, std::move(C)) {}
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
struct Point {
  int x, y;
};
)cpp",
    R"cpp(
std::cout << Point{3, 4} << "\n";
)cpp",
    R"cpp(
struct Person {
  std::string name;
  int age;
  bool admin;
  double score;
};
)cpp",
    R"cpp(
std::cout << Person{"Ada", 36, true, 9.5} << "\n";
)cpp",
    R"cpp(
enum class Color { Red, Green };
)cpp",
    R"cpp(
struct Pixel {
  Point at;
  Color color;
  int rgb[3];
};
)cpp",
    R"cpp(
std::cout << Pixel{{1, 2}, Color::Green, {255, 128, 0}} << "\n";
)cpp",
    R"cpp(
class Secret {
  int hidden = 1;

public:
  int shown = 2;
};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<bool> Unsafe(
      "unsafe",
      llvm::cl::desc("Inject immediately inside the callback to demonstrate "
                     "reentrancy failure"),
      llvm::cl::cat(Options), llvm::cl::init(false));
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

  auto W = std::make_unique<RecordWatcher>();
  RecordWatcher *Watcher = W.get();
  Watcher->Unsafe = Unsafe;
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
                                "error: ");
    return 1;
  }
  auto CI = std::move(*CompilerOrError);
  llvm::Error Err = llvm::Error::success();
  // The protected constructor accepts this consumer but does not install the
  // factory runtime prelude.
  WatchingInterpreter I(std::move(CI), Err, std::move(W));
  if (llvm::Error Error = std::move(Err)) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  Watcher->Interp = &I;

  if (llvm::Error Error = I.ParseAndExecute(R"cpp(
#include <iostream>
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I.ParseAndExecute(R"cpp(
#include <sstream>
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }
  if (llvm::Error Error = I.ParseAndExecute(R"cpp(
#include <string>
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
  }

  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      llvm::outs() << "input: " << Code << "\n";
      llvm::Error InputError2 = I.ParseAndExecute(Code);
      bool InputAccepted1 = !InputError2;
      if (InputError2) {
        llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                    "error: ");
      }
      // A rejected input invalidates its pending declarations; never generate
      // from those pointers.
      if (!InputAccepted1) {
        Watcher->Pending
            .clear(); // the input was rolled back: its classes do not exist
        continue;
      }

      // The input is finished: nothing is parsing, so injecting is safe now.
      std::vector<const CXXRecordDecl *> Todo;
      // The input has finished, so consume its accepted records before
      // submitting generated source.
      Todo.swap(Watcher->Pending);
      for (const CXXRecordDecl *R : Todo) {
        std::string Why;
        std::string PrinterSource2;
        bool Supported = true;

        const ASTContext &Ctx = R->getASTContext();
        std::string Name = R->getQualifiedNameAsString();
        std::string Body = R"cpp(
  os << ")cpp" + Name + R"cpp({"
)cpp";
        bool First = true;
        for (const FieldDecl *F : R->fields()) {
          if (F->getAccess() != AS_public) {
            Why = "field '" + F->getNameAsString() + "' is not public";
            Supported = false;
            break;
          }
          QualType T = F->getType().getCanonicalType();
          std::string Field = R"cpp(v.)cpp" + F->getNameAsString();
          std::string Value;
          if (T->isEnumeralType()) {
            Value = R"cpp(static_cast<long long>()cpp" + Field + R"cpp())cpp";
          } else if (T->isBooleanType()) {
            Value = R"cpp(()cpp" + Field + R"cpp( ? "true" : "false"))cpp";
          } else if (Ctx.getAsConstantArrayType(T)) {
            Value = R"cpp(
[&] {
  std::ostringstream text;
  const char *separator = "";
  text << "{";
  for (const auto &element : )cpp" +
                    Field + R"cpp() {
    text << separator << element;
    separator = ", ";
  }
  text << "}";
  return text.str();
}()
)cpp";
          } else {
            Value = Field;
          }
          Body += std::string(R"cpp(
      << ")cpp") + (First ? "" : ", ") +
                  F->getNameAsString() + R"cpp(=" << )cpp" + Value;
          First = false;
        }
        Body += R"cpp(
      << "}";
  return os;
)cpp";
        if (Supported) {
          PrinterSource2 = R"cpp(
std::ostream &operator<<(std::ostream &os, const )cpp" +
                           Name + R"cpp( &v) {
)cpp" + Body + R"cpp(
}
)cpp";
        }

        std::string Src = PrinterSource2;
        if (Src.empty()) {
          llvm::outs() << "  [inject] skipped " << R->getQualifiedNameAsString()
                       << ": " << Why << "\n";
          continue;
        }
        llvm::outs() << "  [inject] " << Src << "\n";
        // The generated operator is another C++ input and needs its own
        // compilation error handling.
        if (llvm::Error Error = I.ParseAndExecute(Src)) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
      }
    }
  }
  return 0;
}
