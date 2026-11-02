#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/PartialTranslationUnit.h>
#include <clang/Interpreter/Value.h>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace llvm::orc;

namespace {

class LoggingExecutor : public IncrementalExecutor {
public:
  explicit LoggingExecutor(std::unique_ptr<LLJIT> J) : JIT(std::move(J)) {}

  // 1. A new module arrived: hand it to the JIT, remember its ResourceTracker.
  llvm::Error addModule(PartialTranslationUnit &PTU) override {
    llvm::Module &M = *PTU.TheModule;
    std::vector<std::string> Defined;
    for (llvm::Function &F : M) {
      if (!F.isDeclaration()) {
        Defined.push_back(F.getName().str());
      }
    }
    llvm::sort(Defined);
    llvm::outs() << "  [executor] addModule " << M.getName() << ": defines";
    for (const std::string &N : Defined) {
      llvm::outs() << " " << N;
    }
    llvm::outs() << "\n";

    // The round trip: bitcode out of the interpreter's context, into ours.
    llvm::SmallVector<char, 0> Bitcode;
    llvm::raw_svector_ostream OS(Bitcode);
    // Serialize before changing contexts: a Module cannot be paired with an
    // unrelated context lock.
    llvm::WriteBitcodeToFile(M, OS);
    auto Ctx = std::make_unique<llvm::LLVMContext>();
    auto Copy = llvm::parseBitcodeFile(
        llvm::MemoryBufferRef(llvm::StringRef(Bitcode.data(), Bitcode.size()),
                              M.getName()),
        *Ctx);
    if (!Copy) {
      return Copy.takeError();
    }
    PTU.TheModule.reset(); // like OrcIncrementalExecutor: the executor consumes
                           // the module

    // Track this input's allocations so Undo can remove the matching module
    // resources.
    ResourceTrackerSP RT = JIT->getMainJITDylib().createResourceTracker();
    Trackers[&PTU] = RT;
    // Transfer both the recreated module and its owning ThreadSafeContext into
    // LLJIT.
    return JIT->addIRModule(
        RT,
        ThreadSafeModule(std::move(*Copy), ThreadSafeContext(std::move(Ctx))));
  }

  // 2. Undo: drop everything the module defined.
  llvm::Error removeModule(PartialTranslationUnit &PTU) override {
    llvm::outs() << "  [executor] removeModule\n";
    auto It = Trackers.find(&PTU);
    if (It == Trackers.end()) {
      return llvm::Error::success();
    }
    ResourceTrackerSP RT = std::move(It->second);
    Trackers.erase(It);
    return RT->remove();
  }

  // 3. Run the static initializers of everything added so far.
  llvm::Error runCtors() const override {
    llvm::outs() << "  [executor] runCtors\n";
    return JIT->initialize(JIT->getMainJITDylib());
  }

  // 4. Shutdown: run the static destructors.
  llvm::Error cleanUp() override {
    llvm::outs() << "  [executor] cleanUp\n";
    return JIT->deinitialize(JIT->getMainJITDylib());
  }

  llvm::Expected<llvm::orc::ExecutorAddr>
  getSymbolAddress(llvm::StringRef Name, SymbolNameKind Kind) const override {
    llvm::outs() << "  [executor] getSymbolAddress " << Name << " ("
                 << (Kind == IRName ? "IRName" : "LinkerName") << ")\n";
    ExecutionSession &ES = JIT->getExecutionSession();
    auto SO = makeJITDylibSearchOrder({&JIT->getMainJITDylib(),
                                       JIT->getPlatformJITDylib().get(),
                                       JIT->getProcessSymbolsJITDylib().get()});
    // Search the selected dylibs; IR names need target mangling, linker names
    // are already spelled.
    auto Sym = ES.lookup(SO, Kind == LinkerName ? ES.intern(Name)
                                                : JIT->mangleAndIntern(Name));
    if (!Sym) {
      return Sym.takeError();
    }
    return Sym->getAddress();
  }

  // 5. Library loading: make the symbols of a shared library visible to JIT'd
  // code.
  llvm::Error LoadDynamicLibrary(const char *Name) override {
    llvm::outs() << "  [executor] LoadDynamicLibrary "
                 << llvm::sys::path::filename(Name) << "\n";
    auto Gen = DynamicLibrarySearchGenerator::Load(
        Name, JIT->getDataLayout().getGlobalPrefix());
    if (!Gen) {
      return Gen.takeError();
    }
    JIT->getProcessSymbolsJITDylib()->addGenerator(std::move(*Gen));
    return llvm::Error::success();
  }

private:
  std::unique_ptr<LLJIT> JIT;
  llvm::DenseMap<PartialTranslationUnit *, ResourceTrackerSP> Trackers;
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <stdio.h>
)cpp",
    R"cpp(
int answer() {
  return 42;
}
)cpp",
    R"cpp(
struct Bye {
  ~Bye() {
    printf("static destructor ran\n");
  }
} bye;
)cpp",
    R"cpp(
answer()
)cpp",
    R"cpp(
printf("still alive\n");
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");

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

  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  auto ExecutorOrError = LLJITBuilder().create();
  if (!ExecutorOrError) {
    llvm::logAllUnhandledErrors(ExecutorOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  // Supply this executor implementation; the interpreter will invoke its six
  // virtual operations.
  IEB->IE = std::make_unique<LoggingExecutor>(std::move(*ExecutorOrError));

  llvm::outs() << "creating the interpreter (it runs the runtime prelude "
                  "through the executor)\n";
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
      clang::Interpreter::create(std::move(*CompilerOrError), std::move(IEB));
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);

  {
    for (const std::string &Code : SourceInputs) {
      if (llvm::StringRef(Code).starts_with("//")) {
        continue;
      }
      llvm::outs() << "input: " << Code << "\n";
      // Compile this input, then submit its module and run its initializers.
      if (llvm::Error Error = I->ParseAndExecute(Code)) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
    }
  }

  // Remove one accepted input through the API, without a command language.
  if (llvm::Error Error = I->ParseAndExecute(R"cpp(
int scratch = 1;
)cpp")) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }
  // Remove recent accepted inputs and their symbols; earlier writes and I/O
  // remain.
  if (llvm::Error Error = I->Undo()) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  std::string LinkerName;
  llvm::DataLayout TargetLayout(
      I->getASTContext().getTargetInfo().getDataLayoutString());
  if (char Prefix = TargetLayout.getGlobalPrefix()) {
    LinkerName += Prefix;
  }
  LinkerName += "_Z6answerv";
  for (auto [Name, Kind] :
       {std::pair<std::string, const char *>{"_Z6answerv", "IR name"},
        std::pair<std::string, const char *>{LinkerName, "linker name"}}) {
    llvm::outs() << "lookup by " << Kind << " " << Name << ":\n";
    auto Addr = Kind[0] == 'I'
                    ? I->getSymbolAddress(llvm::StringRef(Name))
                    : I->getSymbolAddressFromLinkerName(llvm::StringRef(Name));
    llvm::outs() << "  -> " << (Addr ? "found" : "not found") << "\n";
    if (!Addr) {
      llvm::consumeError(Addr.takeError());
    }
  }

  llvm::outs() << "destroying the interpreter\n";
  I.reset();
  llvm::outs() << "done\n";
  return 0;
}
