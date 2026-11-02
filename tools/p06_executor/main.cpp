// p06_executor -- implement clang::IncrementalExecutor yourself (Section 6.5).
//
// LoggingExecutor owns an LLJIT and logs every call the interpreter makes
// through the six virtuals, so you can watch the protocol:
//
//   Parse + Execute(PTU)     addModule(PTU), then runCtors()
//   Undo()                   removeModule(PTU)
//   getSymbolAddress(name)   getSymbolAddress(name, kind)
//   ~Interpreter()           cleanUp()
//
// The interpreter's llvm::orc::ThreadSafeContext is private, so the module in
// PTU.TheModule cannot be wrapped in a ThreadSafeModule with the right
// context. The way out used here: serialize the module to bitcode and read it
// back into a LLVMContext this executor owns.
//
// Usage: build/bin/p06_executor inputs.repl [-- compiler flags]
#include "repllab.h"

#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Module.h"

#include <algorithm>

using namespace clang;
using namespace llvm::orc;

namespace {

class LoggingExecutor : public IncrementalExecutor {
public:
  static llvm::Expected<std::unique_ptr<LoggingExecutor>> create() {
    auto JIT = LLJITBuilder().create();
    if (!JIT)
      return JIT.takeError();
    return std::unique_ptr<LoggingExecutor>(new LoggingExecutor(std::move(*JIT)));
  }

  // 1. A new module arrived: hand it to the JIT, remember its ResourceTracker.
  llvm::Error addModule(PartialTranslationUnit &PTU) override {
    llvm::Module &M = *PTU.TheModule;
    std::vector<std::string> Defined;
    for (llvm::Function &F : M)
      if (!F.isDeclaration())
        Defined.push_back(F.getName().str());
    llvm::sort(Defined);
    llvm::outs() << "  [executor] addModule " << M.getName() << ": defines";
    for (const std::string &N : Defined)
      llvm::outs() << " " << N;
    llvm::outs() << "\n";

    // The round trip: bitcode out of the interpreter's context, into ours.
    llvm::SmallVector<char, 0> Bitcode;
    llvm::raw_svector_ostream OS(Bitcode);
    llvm::WriteBitcodeToFile(M, OS);
    auto Ctx = std::make_unique<llvm::LLVMContext>();
    auto Copy = llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef(Bitcode.data(), Bitcode.size()), M.getName()), *Ctx);
    if (!Copy)
      return Copy.takeError();
    PTU.TheModule.reset(); // like OrcIncrementalExecutor: the executor consumes the module

    ResourceTrackerSP RT = JIT->getMainJITDylib().createResourceTracker();
    Trackers[&PTU] = RT;
    return JIT->addIRModule(RT, ThreadSafeModule(std::move(*Copy), ThreadSafeContext(std::move(Ctx))));
  }

  // 2. Undo: drop everything the module defined.
  llvm::Error removeModule(PartialTranslationUnit &PTU) override {
    llvm::outs() << "  [executor] removeModule\n";
    auto It = Trackers.find(&PTU);
    if (It == Trackers.end())
      return llvm::Error::success();
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

  llvm::Expected<llvm::orc::ExecutorAddr> getSymbolAddress(llvm::StringRef Name,
                                                           SymbolNameKind Kind) const override {
    llvm::outs() << "  [executor] getSymbolAddress " << Name << " ("
                 << (Kind == IRName ? "IRName" : "LinkerName") << ")\n";
    ExecutionSession &ES = JIT->getExecutionSession();
    auto SO = makeJITDylibSearchOrder(
        {&JIT->getMainJITDylib(), JIT->getPlatformJITDylib().get(), JIT->getProcessSymbolsJITDylib().get()});
    auto Sym = ES.lookup(SO, Kind == LinkerName ? ES.intern(Name) : JIT->mangleAndIntern(Name));
    if (!Sym)
      return Sym.takeError();
    return Sym->getAddress();
  }

  // 5. %lib: make the symbols of a shared library visible to JIT'd code.
  llvm::Error LoadDynamicLibrary(const char *Name) override {
    llvm::outs() << "  [executor] LoadDynamicLibrary " << llvm::sys::path::filename(Name) << "\n";
    auto Gen = DynamicLibrarySearchGenerator::Load(Name, JIT->getDataLayout().getGlobalPrefix());
    if (!Gen)
      return Gen.takeError();
    JIT->getProcessSymbolsJITDylib()->addGenerator(std::move(*Gen));
    return llvm::Error::success();
  }

private:
  explicit LoggingExecutor(std::unique_ptr<LLJIT> J) : JIT(std::move(J)) {}

  std::unique_ptr<LLJIT> JIT;
  llvm::DenseMap<PartialTranslationUnit *, ResourceTrackerSP> Trackers;
};

} // namespace

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_executor: ");
  repllab::Args A = repllab::splitArgs(argc, argv);

  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->IE = ExitOnErr(LoggingExecutor::create());

  llvm::outs() << "creating the interpreter (it runs the runtime prelude through the executor)\n";
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler, std::move(IEB)));

  for (const std::string &File : A.Positional) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      if (Code == "%undo") {
        llvm::outs() << "input: Undo()\n";
        if (llvm::Error E = I->Undo())
          llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ");
        continue;
      }
      llvm::outs() << "input: " << Code << "\n";
      repllab::run(*I, Code);
    }
  }

  std::string LinkerName;
  llvm::DataLayout TargetLayout(I->getASTContext().getTargetInfo().getDataLayoutString());
  if (char Prefix = TargetLayout.getGlobalPrefix())
    LinkerName += Prefix;
  LinkerName += "_Z6answerv";
  for (auto [Name, Kind] : {std::pair<std::string, const char *>{"_Z6answerv", "IR name"},
                           std::pair<std::string, const char *>{LinkerName, "linker name"}}) {
    llvm::outs() << "lookup by " << Kind << " " << Name << ":\n";
    auto Addr = Kind[0] == 'I' ? I->getSymbolAddress(llvm::StringRef(Name))
                               : I->getSymbolAddressFromLinkerName(llvm::StringRef(Name));
    llvm::outs() << "  -> " << (Addr ? "found" : "not found") << "\n";
    if (!Addr)
      llvm::consumeError(Addr.takeError());
  }

  llvm::outs() << "destroying the interpreter\n";
  I.reset();
  llvm::outs() << "done\n";
  return 0;
}
