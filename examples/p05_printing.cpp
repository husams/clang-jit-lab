#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// std::string -> "text"; std::vector<T> -> [a, b, c] (elements pretty-printed
// recursively); everything else: the interpreter's own printing.
static std::string prettyValue(Interpreter &I, const Value &V) {
  if (!V.isValid()) {
    return "<error>";
  }
  // Canonical: `std::string` is a typedef, the pattern below wants what it
  // names.
  // Remove references and aliases to recognize the underlying library class
  // declaration.
  QualType QT =
      V.getType().getNonReferenceType().getCanonicalType().getUnqualifiedType();
  std::string T = QT.getAsString(V.getASTContext().getPrintingPolicy());
  bool IsRef = V.getType()->isReferenceType();
  // Recognise the class by its declaration, not by the spelling of its type.
  const CXXRecordDecl *RD = QT->getAsCXXRecordDecl();
  bool InStd = RD && RD->isInStdNamespace();

  if (IsRef && InStd && RD->getName() == "basic_string") {
    Value S;
    // Ask generated C++ to call c_str() through the public interface, without
    // reading library internals.
    // S receives the pointer result and must be released while the interpreter
    // is alive.
    if (llvm::Error Error = I.ParseAndExecute(
            (R"cpp(((const )cpp" + (T) + R"cpp( *))cpp" +
             std::to_string((uintptr_t)V.getPtr()) + R"cpp(UL))cpp") +
                R"cpp(->c_str())cpp",
            &S)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    return std::string("\"") + S.convertTo<const char *>() + "\"";
  }
  if (IsRef && InStd && RD->getName() == "vector") {
    // The element type comes from the AST, not from string surgery.
    const auto *Spec = llvm::cast<ClassTemplateSpecializationDecl>(RD);
    std::string Elt = Spec->getTemplateArgs()[0].getAsType().getAsString(
        V.getASTContext().getPrintingPolicy());
    Value NV;
    if (llvm::Error Error = I.ParseAndExecute(
            (R"cpp(((const )cpp" + (T) + R"cpp( *))cpp" +
             std::to_string((uintptr_t)V.getPtr()) + R"cpp(UL))cpp") +
                R"cpp(->size())cpp",
            &NV)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    size_t N = NV.convertTo<size_t>();
    std::string Out = "[";
    for (size_t K = 0; K < N; ++K) {
      Value E;
      if (llvm::Error Error = I.ParseAndExecute(
              R"cpp((*)cpp" +
                  (R"cpp(((const )cpp" + (T) + R"cpp( *))cpp" +
                   std::to_string((uintptr_t)V.getPtr()) + R"cpp(UL))cpp") +
                  R"cpp()[)cpp" + std::to_string(K) + R"cpp(])cpp",
              &E)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      Out += (K ? ", " : "") + prettyValue(I, E);
    }
    return Out + "]";
  }
  // Fallback: the built-in printer, minus the "(type) " prefix.
  std::string PrintedValue;
  llvm::raw_string_ostream OutputForPrintedValue(PrintedValue);
  // Use Value's data formatter with this stream, omitting the combined type
  // prefix.
  V.printData(OutputForPrintedValue);
  return PrintedValue;
}

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <string>
)cpp",
    R"cpp(
#include <vector>
)cpp",
    R"cpp(
#include <cstdio>
)cpp",
    R"cpp(
enum Color { Red, Green = 5 };
)cpp",
    R"cpp(
struct P {
  int a;
  int b;
};
)cpp",
    R"cpp(
int arr[3] = {7, 8, 9};
)cpp",
    R"cpp(
double grid[2][2] = {{1, 2}, {3, 4}};
)cpp",
    R"cpp(
float f = 1.5f;
)cpp",
    R"cpp(
double d = 0.1;
)cpp",
    R"cpp(
const char *cs = "hi";
)cpp",
    R"cpp(
char ch = 'A';
)cpp",
    R"cpp(
int *ip = &arr[1];
)cpp",
    R"cpp(
void twice(int x) {
  printf("%d\n", 2 * x);
}
)cpp",
    R"cpp(
void (*fp)(int) = twice;
)cpp",
    R"cpp(
std::string name = "C++ JIT";
)cpp",
    R"cpp(
std::vector<int> nums = {3, 1, 4, 1, 5};
)cpp",
    R"cpp(
std::vector<std::string> words = {"a", "bb", "ccc"};
)cpp",
    R"cpp(
P p{1, 2};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: apis, capture, pretty"),
      llvm::cl::cat(Options), llvm::cl::init("pretty"));
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

  // nodiscard on size() and operator[] would warn on every probe we evaluate.
  CompilerFlags.insert(CompilerFlags.begin(), "-Wno-unused-result");
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
  for (const std::string &Code : SourceInputs) {
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }

  if (Mode == "apis") {
    for (const char *Expr : {R"cpp(
f
)cpp",
                             R"cpp(
d
)cpp",
                             R"cpp(
cs
)cpp",
                             R"cpp(
ch
)cpp",
                             R"cpp(
(Color)5
)cpp",
                             R"cpp(
(Color)3
)cpp",
                             R"cpp(
arr
)cpp",
                             R"cpp(
grid
)cpp",
                             R"cpp(
ip
)cpp",
                             R"cpp(
fp
)cpp",
                             R"cpp(
twice
)cpp",
                             R"cpp(
p
)cpp",
                             R"cpp(
name
)cpp"}) {
      Value V;
      if (llvm::Error Error = I->ParseAndExecute(Expr, &V)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      std::string PrintedValue2;
      llvm::raw_string_ostream OutputForPrintedValue2(PrintedValue2);
      V.printType(OutputForPrintedValue2);
      std::string PrintedValue3;
      llvm::raw_string_ostream OutputForPrintedValue3(PrintedValue3);
      V.printData(OutputForPrintedValue3);
      llvm::outs() << llvm::format("%-9s", Expr) << "type: " << PrintedValue2
                   << "\n"
                   << llvm::format("%-9s", "") << "data: " << PrintedValue3
                   << "\n";
    }
  } else if (Mode == "capture") {
    llvm::outs() << "without a Value, the interpreter prints:\n";
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
nums.size()
)cpp")) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "with a Value, nothing is printed:\n";
    Value V;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
nums.size()
)cpp",
                                               &V)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "then print(), dump() and printData() are ours to call:\n";
    // print writes type and data; with a captured Value, presentation belongs
    // to the host.
    V.print(llvm::outs());
    V.dump();
    V.printData(llvm::outs());
    llvm::outs() << "\n";
    llvm::outs() << "a declaration prints nothing either way: ";
    Value W;
    if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int unused = 0;
)cpp",
                                               &W)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    }
    llvm::outs() << "isValid=" << W.isValid() << "\n";
  } else if (Mode == "pretty") {
    for (const char *Expr : {R"cpp(
name
)cpp",
                             R"cpp(
nums
)cpp",
                             R"cpp(
words
)cpp",
                             R"cpp(
p
)cpp",
                             R"cpp(
name.size()
)cpp",
                             R"cpp(
arr
)cpp"}) {
      Value V;
      if (llvm::Error Error = I->ParseAndExecute(Expr, &V)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      std::string PrintedValue4;
      llvm::raw_string_ostream OutputForPrintedValue4(PrintedValue4);
      (V.*(&Value::print))(OutputForPrintedValue4);
      llvm::outs() << llvm::format("%-12s", Expr)
                   << "default: " << PrintedValue4;
      llvm::outs() << llvm::format("%-12s", "")
                   << "pretty:  " << prettyValue(*I, V) << "\n";
    }
  }
  return 0;
}
