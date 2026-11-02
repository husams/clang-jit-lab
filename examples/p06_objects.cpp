#include <algorithm>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace llvm::orc;

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
int sum_to(int n) {
  int s = 0;
  for (int i = 1; i <= n; ++i)
    s += i * i;
  return s;
}
)cpp",
    R"cpp(
sum_to(10)
)cpp",
    R"cpp(
int twice(int v) {
  return 2 * v;
}
)cpp",
    R"cpp(
twice(sum_to(3))
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> OutDir(
      "output-dir", llvm::cl::desc("Directory for captured object files"),
      llvm::cl::cat(Options), llvm::cl::init("objects"));
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

  if (llvm::Error Error =
          llvm::errorCodeToError(llvm::sys::fs::create_directories(OutDir))) {
    llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
    return 1;
  }

  // Filled by the JIT. In-process, the callback has been observed on the
  // calling thread only; the mutex is defensive (other executors may return
  // buffers from worker threads). The order is what varies: one lookup can
  // materialize several lazy modules, in hash order, so each batch is sorted
  // before naming the files.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Objs;
  std::mutex ObjsMutex;

  auto JB = std::make_unique<LLJITBuilder>();
  // Install the object-layer hook once the JIT exists, before input
  // materialization.
  JB->setNotifyCreatedCallback([&](LLJIT &J) -> llvm::Error {
    // On macOS and Linux the object layer is JITLink's ObjectLinkingLayer
    // (RuntimeDyld-based RTDyldObjectLinkingLayer has a different hook).
    auto *OL = llvm::dyn_cast<ObjectLinkingLayer>(&J.getObjLinkingLayer());
    if (!OL) {
      return llvm::make_error<llvm::StringError>(
          "not an ObjectLinkingLayer", llvm::inconvertibleErrorCode());
    }
    // JITLink returns the original object buffer after linking; executable
    // memory is separate.
    // Retain this buffer for symbol inspection or disassembly, not as a
    // callable address.
    OL->setReturnObjectBuffer([&](std::unique_ptr<llvm::MemoryBuffer> MB) {
      std::lock_guard<std::mutex> Lock(ObjsMutex);
      Objs.push_back(std::move(MB));
    });
    return llvm::Error::success();
  });
  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->JITBuilder = std::move(JB);
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

  // The external symbols an object defines (internal helpers hidden), sorted.

  size_t Written = 0;

  {

    std::lock_guard<std::mutex> Lock(ObjsMutex);
    // Objects with defined symbols first, by name; the rest (statement-only
    // modules) after them.
    std::stable_sort(
        Objs.begin() + Written, Objs.end(), [&](const auto &A, const auto &B) {
          std::vector<std::string> ObjectSymbols701;
          auto ObjectForObjectSymbols701 =
              llvm::object::ObjectFile::createObjectFile(
                  (*A).getMemBufferRef());
          if (!ObjectForObjectSymbols701) {
            llvm::logAllUnhandledErrors(ObjectForObjectSymbols701.takeError(),
                                        llvm::errs(), "object: ");
          } else {
            for (const llvm::object::SymbolRef &SymbolForObjectSymbols701 :
                 (*ObjectForObjectSymbols701)->symbols()) {
              auto FlagsForObjectSymbols701 =
                  SymbolForObjectSymbols701.getFlags();
              if (!FlagsForObjectSymbols701) {
                llvm::logAllUnhandledErrors(
                    FlagsForObjectSymbols701.takeError(), llvm::errs(),
                    "symbol: ");
                continue;
              }
              if (!(*FlagsForObjectSymbols701 &
                    llvm::object::SymbolRef::SF_Global) ||
                  (*FlagsForObjectSymbols701 &
                   llvm::object::SymbolRef::SF_Undefined)) {
                continue;
              }
              auto NameForObjectSymbols701 =
                  SymbolForObjectSymbols701.getName();
              if (!NameForObjectSymbols701) {
                llvm::logAllUnhandledErrors(NameForObjectSymbols701.takeError(),
                                            llvm::errs(), "symbol name: ");
                continue;
              }
              if (!NameForObjectSymbols701->starts_with("__GLOBAL__") &&
                  !NameForObjectSymbols701->starts_with("____orc")) {
                ObjectSymbols701.push_back(NameForObjectSymbols701->str());
              }
            }
          }
          llvm::sort(ObjectSymbols701);
          std::vector<std::string> ObjectSymbols702;
          auto ObjectForObjectSymbols702 =
              llvm::object::ObjectFile::createObjectFile(
                  (*B).getMemBufferRef());
          if (!ObjectForObjectSymbols702) {
            llvm::logAllUnhandledErrors(ObjectForObjectSymbols702.takeError(),
                                        llvm::errs(), "object: ");
          } else {
            for (const llvm::object::SymbolRef &SymbolForObjectSymbols702 :
                 (*ObjectForObjectSymbols702)->symbols()) {
              auto FlagsForObjectSymbols702 =
                  SymbolForObjectSymbols702.getFlags();
              if (!FlagsForObjectSymbols702) {
                llvm::logAllUnhandledErrors(
                    FlagsForObjectSymbols702.takeError(), llvm::errs(),
                    "symbol: ");
                continue;
              }
              if (!(*FlagsForObjectSymbols702 &
                    llvm::object::SymbolRef::SF_Global) ||
                  (*FlagsForObjectSymbols702 &
                   llvm::object::SymbolRef::SF_Undefined)) {
                continue;
              }
              auto NameForObjectSymbols702 =
                  SymbolForObjectSymbols702.getName();
              if (!NameForObjectSymbols702) {
                llvm::logAllUnhandledErrors(NameForObjectSymbols702.takeError(),
                                            llvm::errs(), "symbol name: ");
                continue;
              }
              if (!NameForObjectSymbols702->starts_with("__GLOBAL__") &&
                  !NameForObjectSymbols702->starts_with("____orc")) {
                ObjectSymbols702.push_back(NameForObjectSymbols702->str());
              }
            }
          }
          llvm::sort(ObjectSymbols702);
          auto DA = ObjectSymbols701, DB = ObjectSymbols702;
          if (DA.empty() != DB.empty()) {
            return DB.empty();
          }
          return DA < DB;
        });
    for (; Written < Objs.size(); ++Written) {
      llvm::SmallString<128> Path(OutDir);
      llvm::sys::path::append(Path,
                              llvm::formatv("{0:02}.o", Written + 1).str());
      std::error_code EC;
      llvm::raw_fd_ostream OS(Path, EC);
      if (EC) {
        if (llvm::Error Error = llvm::errorCodeToError(EC)) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
          return 1;
        }
      }
      OS << Objs[Written]->getBuffer();
      // Mach-O 64-bit magic, little endian: CF FA ED FE.
      bool MachO = Objs[Written]->getBuffer().take_front(4) ==
                   llvm::StringRef("\xcf\xfa\xed\xfe", 4);
      bool ELF =
          Objs[Written]->getBuffer().take_front(4) == llvm::StringRef("\x7f"
                                                                      "ELF",
                                                                      4);
      llvm::outs() << "  -> " << Path
                   << (MachO ? "  (Mach-O object)"
                       : ELF ? "  (ELF object)"
                             : "  (other object)")
                   << "\n";
      std::vector<std::string> ObjectSymbols703;
      auto ObjectForObjectSymbols703 =
          llvm::object::ObjectFile::createObjectFile(
              (*Objs[Written]).getMemBufferRef());
      if (!ObjectForObjectSymbols703) {
        llvm::logAllUnhandledErrors(ObjectForObjectSymbols703.takeError(),
                                    llvm::errs(), "object: ");
      } else {
        for (const llvm::object::SymbolRef &SymbolForObjectSymbols703 :
             (*ObjectForObjectSymbols703)->symbols()) {
          auto FlagsForObjectSymbols703 = SymbolForObjectSymbols703.getFlags();
          if (!FlagsForObjectSymbols703) {
            llvm::logAllUnhandledErrors(FlagsForObjectSymbols703.takeError(),
                                        llvm::errs(), "symbol: ");
            continue;
          }
          if (!(*FlagsForObjectSymbols703 &
                llvm::object::SymbolRef::SF_Global) ||
              (*FlagsForObjectSymbols703 &
               llvm::object::SymbolRef::SF_Undefined)) {
            continue;
          }
          auto NameForObjectSymbols703 = SymbolForObjectSymbols703.getName();
          if (!NameForObjectSymbols703) {
            llvm::logAllUnhandledErrors(NameForObjectSymbols703.takeError(),
                                        llvm::errs(), "symbol name: ");
            continue;
          }
          if (!NameForObjectSymbols703->starts_with("__GLOBAL__") &&
              !NameForObjectSymbols703->starts_with("____orc")) {
            ObjectSymbols703.push_back(NameForObjectSymbols703->str());
          }
        }
      }
      llvm::sort(ObjectSymbols703);
      for (const std::string &N : ObjectSymbols703) {
        llvm::outs() << "       defines " << N << "\n";
      }
    }

  } // objects produced while the interpreter started (the prelude)
  llvm::outs() << "objects after startup: " << Objs.size() << "\n";
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
      {

        std::lock_guard<std::mutex> Lock(ObjsMutex);
        // Objects with defined symbols first, by name; the rest (statement-only
        // modules) after them.
        std::stable_sort(
            Objs.begin() + Written, Objs.end(),
            [&](const auto &A, const auto &B) {
              std::vector<std::string> ObjectSymbols704;
              auto ObjectForObjectSymbols704 =
                  llvm::object::ObjectFile::createObjectFile(
                      (*A).getMemBufferRef());
              if (!ObjectForObjectSymbols704) {
                llvm::logAllUnhandledErrors(
                    ObjectForObjectSymbols704.takeError(), llvm::errs(),
                    "object: ");
              } else {
                for (const llvm::object::SymbolRef &SymbolForObjectSymbols704 :
                     (*ObjectForObjectSymbols704)->symbols()) {
                  auto FlagsForObjectSymbols704 =
                      SymbolForObjectSymbols704.getFlags();
                  if (!FlagsForObjectSymbols704) {
                    llvm::logAllUnhandledErrors(
                        FlagsForObjectSymbols704.takeError(), llvm::errs(),
                        "symbol: ");
                    continue;
                  }
                  if (!(*FlagsForObjectSymbols704 &
                        llvm::object::SymbolRef::SF_Global) ||
                      (*FlagsForObjectSymbols704 &
                       llvm::object::SymbolRef::SF_Undefined)) {
                    continue;
                  }
                  auto NameForObjectSymbols704 =
                      SymbolForObjectSymbols704.getName();
                  if (!NameForObjectSymbols704) {
                    llvm::logAllUnhandledErrors(
                        NameForObjectSymbols704.takeError(), llvm::errs(),
                        "symbol name: ");
                    continue;
                  }
                  if (!NameForObjectSymbols704->starts_with("__GLOBAL__") &&
                      !NameForObjectSymbols704->starts_with("____orc")) {
                    ObjectSymbols704.push_back(NameForObjectSymbols704->str());
                  }
                }
              }
              llvm::sort(ObjectSymbols704);
              std::vector<std::string> ObjectSymbols705;
              auto ObjectForObjectSymbols705 =
                  llvm::object::ObjectFile::createObjectFile(
                      (*B).getMemBufferRef());
              if (!ObjectForObjectSymbols705) {
                llvm::logAllUnhandledErrors(
                    ObjectForObjectSymbols705.takeError(), llvm::errs(),
                    "object: ");
              } else {
                for (const llvm::object::SymbolRef &SymbolForObjectSymbols705 :
                     (*ObjectForObjectSymbols705)->symbols()) {
                  auto FlagsForObjectSymbols705 =
                      SymbolForObjectSymbols705.getFlags();
                  if (!FlagsForObjectSymbols705) {
                    llvm::logAllUnhandledErrors(
                        FlagsForObjectSymbols705.takeError(), llvm::errs(),
                        "symbol: ");
                    continue;
                  }
                  if (!(*FlagsForObjectSymbols705 &
                        llvm::object::SymbolRef::SF_Global) ||
                      (*FlagsForObjectSymbols705 &
                       llvm::object::SymbolRef::SF_Undefined)) {
                    continue;
                  }
                  auto NameForObjectSymbols705 =
                      SymbolForObjectSymbols705.getName();
                  if (!NameForObjectSymbols705) {
                    llvm::logAllUnhandledErrors(
                        NameForObjectSymbols705.takeError(), llvm::errs(),
                        "symbol name: ");
                    continue;
                  }
                  if (!NameForObjectSymbols705->starts_with("__GLOBAL__") &&
                      !NameForObjectSymbols705->starts_with("____orc")) {
                    ObjectSymbols705.push_back(NameForObjectSymbols705->str());
                  }
                }
              }
              llvm::sort(ObjectSymbols705);
              auto DA = ObjectSymbols704, DB = ObjectSymbols705;
              if (DA.empty() != DB.empty()) {
                return DB.empty();
              }
              return DA < DB;
            });
        for (; Written < Objs.size(); ++Written) {
          llvm::SmallString<128> Path(OutDir);
          llvm::sys::path::append(Path,
                                  llvm::formatv("{0:02}.o", Written + 1).str());
          std::error_code EC;
          llvm::raw_fd_ostream OS(Path, EC);
          if (EC) {
            if (llvm::Error Error = llvm::errorCodeToError(EC)) {
              llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                          "error: ");
              return 1;
            }
          }
          OS << Objs[Written]->getBuffer();
          // Mach-O 64-bit magic, little endian: CF FA ED FE.
          bool MachO = Objs[Written]->getBuffer().take_front(4) ==
                       llvm::StringRef("\xcf\xfa\xed\xfe", 4);
          bool ELF =
              Objs[Written]->getBuffer().take_front(4) == llvm::StringRef("\x7f"
                                                                          "ELF",
                                                                          4);
          llvm::outs() << "  -> " << Path
                       << (MachO ? "  (Mach-O object)"
                           : ELF ? "  (ELF object)"
                                 : "  (other object)")
                       << "\n";
          std::vector<std::string> ObjectSymbols706;
          auto ObjectForObjectSymbols706 =
              llvm::object::ObjectFile::createObjectFile(
                  (*Objs[Written]).getMemBufferRef());
          if (!ObjectForObjectSymbols706) {
            llvm::logAllUnhandledErrors(ObjectForObjectSymbols706.takeError(),
                                        llvm::errs(), "object: ");
          } else {
            for (const llvm::object::SymbolRef &SymbolForObjectSymbols706 :
                 (*ObjectForObjectSymbols706)->symbols()) {
              auto FlagsForObjectSymbols706 =
                  SymbolForObjectSymbols706.getFlags();
              if (!FlagsForObjectSymbols706) {
                llvm::logAllUnhandledErrors(
                    FlagsForObjectSymbols706.takeError(), llvm::errs(),
                    "symbol: ");
                continue;
              }
              if (!(*FlagsForObjectSymbols706 &
                    llvm::object::SymbolRef::SF_Global) ||
                  (*FlagsForObjectSymbols706 &
                   llvm::object::SymbolRef::SF_Undefined)) {
                continue;
              }
              auto NameForObjectSymbols706 =
                  SymbolForObjectSymbols706.getName();
              if (!NameForObjectSymbols706) {
                llvm::logAllUnhandledErrors(NameForObjectSymbols706.takeError(),
                                            llvm::errs(), "symbol name: ");
                continue;
              }
              if (!NameForObjectSymbols706->starts_with("__GLOBAL__") &&
                  !NameForObjectSymbols706->starts_with("____orc")) {
                ObjectSymbols706.push_back(NameForObjectSymbols706->str());
              }
            }
          }
          llvm::sort(ObjectSymbols706);
          for (const std::string &N : ObjectSymbols706) {
            llvm::outs() << "       defines " << N << "\n";
          }
        }
      }
    }
  }
  return 0;
}
