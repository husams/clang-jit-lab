#include <algorithm>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/CodeCompletion.h>
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

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <string>
)cpp",
    R"cpp(
#include <vector>
)cpp",
    R"cpp(
int demo_counter = 0;
)cpp",
    R"cpp(
int demo_total(int a, int b) {
  return a + b;
}
)cpp",
    R"cpp(
struct Point {
  int x;
  int y;
  int norm2() const {
    return x * x + y * y;
  }
};
)cpp",
    R"cpp(
Point demo_point{3, 4};
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<unsigned> Limit("limit",
                                llvm::cl::desc("Maximum completion results"),
                                llvm::cl::cat(Options), llvm::cl::init(8));
  llvm::cl::list<std::string> Prefixes(
      "prefix", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Completion prefix; repeat for several queries"),
      llvm::cl::cat(Options));
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

  std::vector<std::string> Pos(Prefixes.begin(), Prefixes.end());
  if (Pos.empty()) {
    Pos = {"demo_", "demo_point."};
  }

  // The main interpreter: holds the session's declarations.
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
  std::unique_ptr<Interpreter> Main = std::move(*InterpreterOrError);
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = Main->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }

  for (size_t K = 0; K < Pos.size(); ++K) {
    llvm::StringRef Buffer = Pos[K];
    // A fresh CompilerInstance + Interpreter for every request.
    std::vector<const char *> CompilerArguments2;
    for (const std::string &Flag : CompilerFlags) {
      CompilerArguments2.push_back(Flag.c_str());
    }
    clang::IncrementalCompilerBuilder CompilerBuilder2;
    // Give each completion request a separate frontend so its unfinished parse
    // stays isolated.
    CompilerBuilder2.SetCompilerArgs(CompilerArguments2);

    auto CompilerOrError2 = CompilerBuilder2.CreateCpp();
    if (!CompilerOrError2) {
      llvm::logAllUnhandledErrors(CompilerOrError2.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    std::unique_ptr<CompilerInstance> CI = std::move(*CompilerOrError2);
    auto InterpreterOrError2 = Interpreter::create(std::move(CI));
    if (!InterpreterOrError2) {
      llvm::logAllUnhandledErrors(InterpreterOrError2.takeError(), llvm::errs(),
                                  "error: ");
      return 1;
    }
    std::unique_ptr<Interpreter> Fresh = std::move(*InterpreterOrError2);

    unsigned Line = std::count(Buffer.begin(), Buffer.end(), '\n') + 1;
    unsigned Col =
        Buffer.size() + 1; // as ClangRepl.cpp: Pos + 1 (single-line buffers)

    ReplCodeCompleter CC;
    std::vector<std::string> Results;
    // Complete Buffer at the 1-based cursor in Fresh, importing declarations
    // from Main.
    // The parent supplies session knowledge; this request does not execute
    // Buffer.
    CC.codeComplete(Fresh->getCompilerInstance(), Buffer, Line, Col,
                    Main->getCompilerInstance(), Results);

    // ClangRepl.cpp keeps the candidates that start with Prefix and inserts
    // only the part after it.
    std::vector<std::string> Matches;
    for (const std::string &R : Results) {
      if (R.find(CC.Prefix) == 0) {
        Matches.push_back(R);
      }
    }
    std::sort(Matches.begin(), Matches.end());
    Matches.erase(std::unique(Matches.begin(), Matches.end()), Matches.end());

    llvm::outs() << "buffer: '" << Buffer << "'\n";
    llvm::outs() << "  Prefix: '" << CC.Prefix
                 << "'  raw results: " << Results.size()
                 << "  matching: " << Matches.size() << "\n";
    for (size_t R = 0; R < Matches.size() && R < Limit; ++R) {
      llvm::outs() << "  " << Matches[R] << "   (inserts '"
                   << Matches[R].substr(CC.Prefix.size()) << "')\n";
    }
  }
  return 0;
}
