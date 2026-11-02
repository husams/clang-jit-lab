// p06_objects -- keep the object file the JIT produces for every input
// (Section 6.4). ObjectLinkingLayer hands each object buffer back through
// setReturnObjectBuffer once JITLink is done with it; we write it to disk.
//
// Usage: build/bin/p06_objects outdir inputs.repl [-- compiler flags]
//   outdir/NN.o   the NNth object, in the order the JIT finished linking them
#include "repllab.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <mutex>

using namespace clang;
using namespace llvm::orc;

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p06_objects: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 2) {
    llvm::errs() << "usage: p06_objects outdir inputs.repl [-- compiler flags]\n";
    return 2;
  }
  std::string OutDir = A.Positional[0];
  ExitOnErr(llvm::errorCodeToError(llvm::sys::fs::create_directories(OutDir)));

  // Filled by the JIT. In-process, the callback has been observed on the
  // calling thread only; the mutex is defensive (other executors may return
  // buffers from worker threads). The order is what varies: one lookup can
  // materialize several lazy modules, in hash order, so each batch is sorted
  // before naming the files.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Objs;
  std::mutex ObjsMutex;

  auto JB = std::make_unique<LLJITBuilder>();
  JB->setNotifyCreatedCallback([&](LLJIT &J) -> llvm::Error {
    // On macOS and Linux the object layer is JITLink's ObjectLinkingLayer
    // (RuntimeDyld-based RTDyldObjectLinkingLayer has a different hook).
    auto *OL = llvm::dyn_cast<ObjectLinkingLayer>(&J.getObjLinkingLayer());
    if (!OL)
      return llvm::make_error<llvm::StringError>("not an ObjectLinkingLayer", llvm::inconvertibleErrorCode());
    OL->setReturnObjectBuffer([&](std::unique_ptr<llvm::MemoryBuffer> MB) {
      std::lock_guard<std::mutex> Lock(ObjsMutex);
      Objs.push_back(std::move(MB));
    });
    return llvm::Error::success();
  });
  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->JITBuilder = std::move(JB);
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler, std::move(IEB)));

  // The external symbols an object defines (internal helpers hidden), sorted.
  auto definedSymbols = [](const llvm::MemoryBuffer &MB) {
    std::vector<std::string> Defined;
    auto Obj = llvm::object::ObjectFile::createObjectFile(MB.getMemBufferRef());
    if (!Obj) {
      llvm::consumeError(Obj.takeError());
      return Defined;
    }
    for (const llvm::object::SymbolRef &Sym : (*Obj)->symbols()) {
      auto Flags = Sym.getFlags(); // Expected<uint32_t> in LLVM 22
      if (!Flags) {
        llvm::consumeError(Flags.takeError());
        continue;
      }
      if (!(*Flags & llvm::object::SymbolRef::SF_Global) || (*Flags & llvm::object::SymbolRef::SF_Undefined))
        continue;
      auto Name = Sym.getName();
      if (!Name) {
        llvm::consumeError(Name.takeError());
        continue;
      }
      if (!Name->starts_with("__GLOBAL__") && !Name->starts_with("____orc"))
        Defined.push_back(Name->str());
    }
    llvm::sort(Defined);
    return Defined;
  };

  size_t Written = 0;
  auto flush = [&]() {
    std::lock_guard<std::mutex> Lock(ObjsMutex);
    // Objects with defined symbols first, by name; the rest (statement-only
    // modules) after them.
    std::stable_sort(Objs.begin() + Written, Objs.end(), [&](const auto &A, const auto &B) {
      auto DA = definedSymbols(*A), DB = definedSymbols(*B);
      if (DA.empty() != DB.empty())
        return DB.empty();
      return DA < DB;
    });
    for (; Written < Objs.size(); ++Written) {
      llvm::SmallString<128> Path(OutDir);
      llvm::sys::path::append(Path, llvm::formatv("{0:02}.o", Written + 1).str());
      std::error_code EC;
      llvm::raw_fd_ostream OS(Path, EC);
      if (EC)
        ExitOnErr(llvm::errorCodeToError(EC));
      OS << Objs[Written]->getBuffer();
      // Mach-O 64-bit magic, little endian: CF FA ED FE.
      bool MachO = Objs[Written]->getBuffer().take_front(4) == llvm::StringRef("\xcf\xfa\xed\xfe", 4);
      bool ELF = Objs[Written]->getBuffer().take_front(4) == llvm::StringRef("\x7f" "ELF", 4);
      llvm::outs() << "  -> " << Path
                   << (MachO ? "  (Mach-O object)" : ELF ? "  (ELF object)" : "  (other object)") << "\n";
      for (const std::string &N : definedSymbols(*Objs[Written]))
        llvm::outs() << "       defines " << N << "\n";
    }
  };

  flush(); // objects produced while the interpreter started (the prelude)
  llvm::outs() << "objects after startup: " << Objs.size() << "\n";
  for (const std::string &File : llvm::ArrayRef(A.Positional).drop_front()) {
    for (const std::string &Code : repllab::replInputs(repllab::readFile(File))) {
      if (llvm::StringRef(Code).starts_with("//"))
        continue;
      llvm::outs() << "input: " << Code << "\n";
      repllab::run(*I, Code);
      flush();
    }
  }
  return 0;
}
