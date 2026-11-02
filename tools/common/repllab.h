// repllab.h -- shared helpers for the clang-repl lab tools (Parts 4-7).
//
// Header-only, included as "repllab.h". Everything lives in namespace repllab.
//
//   init()                     native target + asm printer/parser, and
//                              unbuffered stdout / llvm::outs() so that JIT
//                              printf output and tool output stay in order
//   platformArgs()             -resource-dir, Homebrew libc++ and the macOS SDK:
//                              what an embedded interpreter needs to find
//                              <stddef.h>, <vector>, <stdio.h> (Section 4.3)
//   makeCompiler(args, cb)     IncrementalCompilerBuilder::CreateCpp() with
//                              platformArgs() + args
//   makeInterpreter(args, IEB) Interpreter::create() on top of makeCompiler()
//   run(I, code, V)            ParseAndExecute; prints "error: ..." and returns
//                              false instead of exiting
//   valueString(V)             Value::print() into a std::string
//   maskAddrs(s)               0x + 7 or more hex digits -> 0xADDR
//   splitArgs(argc, argv)      positional arguments / compiler flags after "--"
//   readFile(path)             whole file as a string (exits on error)
//   replInputs(text)           split a .repl script into inputs with clang-repl's
//                              continuation rule (trailing backslash continues),
//                              skipping blank lines as scripts/session.py does
//                              (typed into clang-repl, a blank line is an empty
//                              input of its own)
//
// Set REPLLAB_RAW=1 in the environment to skip platformArgs() (Section 4.3
// uses that to show what the flags are for).
//
// Rules: additions are append-only. Do not change the behaviour of an existing
// helper; documented outputs of Parts 4-7 depend on it (scripts/doccheck.py).

#ifndef REPLLAB_H
#define REPLLAB_H

#include "clang/AST/ASTContext.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/Value.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if __has_include("repllab_platform.h")
#include "repllab_platform.h"
#endif

#ifndef REPLLAB_LLVM_PREFIX
#define REPLLAB_LLVM_PREFIX "/opt/homebrew/opt/llvm"
#endif
#ifndef REPLLAB_LAB_ROOT
#define REPLLAB_LAB_ROOT "."
#endif
#ifndef REPLLAB_RESOURCE_DIR
#define REPLLAB_RESOURCE_DIR REPLLAB_LLVM_PREFIX "/lib/clang/22"
#endif

namespace repllab {

// The JIT needs the native target and its asm printer (and the asm parser for
// inline asm). Unbuffered output keeps the tool's llvm::outs() and the JIT'd
// code's printf/std::cout in program order even when piped.
inline void init() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  llvm::outs().SetUnbuffered();
}

inline const std::string &sdkPath() {
  static const std::string Path = [] {
    std::string Out;
#ifdef __APPLE__
    if (FILE *P = popen("xcrun --show-sdk-path 2>/dev/null", "r")) {
      char Buf[1024];
      while (fgets(Buf, sizeof Buf, P))
        Out += Buf;
      pclose(P);
    }
#endif
    while (!Out.empty() && (Out.back() == '\n' || Out.back() == '\r'))
      Out.pop_back();
    return Out;
  }();
  return Path;
}

inline bool rawMode() {
  const char *Raw = std::getenv("REPLLAB_RAW");
  return Raw && *Raw == '1';
}

// clang-repl finds these itself because it lives in $(brew --prefix llvm)/bin:
// the driver derives the resource dir and libc++ from the executable's path.
// A tool in build/bin has to say where they are.
inline std::vector<std::string> platformArgs() {
  if (rawMode())
    return {};
  std::vector<std::string> A = {"-resource-dir", REPLLAB_RESOURCE_DIR};
#ifdef __APPLE__
  A.insert(A.end(), {"-nostdinc++", "-isystem",
                                REPLLAB_LLVM_PREFIX "/include/c++/v1"});
  if (!sdkPath().empty()) {
    A.push_back("-isysroot");
    A.push_back(sdkPath());
  }
#elif defined(REPLLAB_HOST_CXX_INCLUDE_DIRS)
  // Linux distro Clang normally uses libstdc++. Reuse the host's discovered
  // paths rather than forcing Homebrew's libc++ directory into that compiler.
  A.push_back("-nostdinc++");
  llvm::SmallVector<llvm::StringRef, 16> Dirs;
  llvm::StringRef(REPLLAB_HOST_CXX_INCLUDE_DIRS).split(Dirs, ';', -1, false);
  for (llvm::StringRef Dir : Dirs) {
    A.push_back("-isystem");
    A.push_back(Dir.str());
  }
#endif
  return A;
}

using DriverCallback = std::function<llvm::Error(const clang::driver::Compilation &)>;

// IncrementalCompilerBuilder keeps `const char *` pointers to the arguments,
// so the strings must outlive CreateCpp(); they live in `All` until it returns.
inline llvm::Expected<std::unique_ptr<clang::CompilerInstance>>
makeCompiler(const std::vector<std::string> &UserArgs = {}, DriverCallback CB = nullptr) {
  std::vector<std::string> All = platformArgs();
  All.insert(All.end(), UserArgs.begin(), UserArgs.end());
  std::vector<const char *> Argv;
  for (const std::string &S : All)
    Argv.push_back(S.c_str());
  clang::IncrementalCompilerBuilder Builder;
  Builder.SetCompilerArgs(Argv);
  if (CB)
    Builder.SetDriverCompilationCallback(CB);
  return Builder.CreateCpp();
}

// Out-of-process builders need the ORC runtime path, which the builder finds
// through a driver callback (exactly what clang-repl does).
inline llvm::Expected<std::unique_ptr<clang::Interpreter>>
makeInterpreter(const std::vector<std::string> &UserArgs = {},
                std::unique_ptr<clang::IncrementalExecutorBuilder> IEB = nullptr) {
  DriverCallback CB;
  if (IEB && IEB->IsOutOfProcess && IEB->OrcRuntimePath.empty()) {
    CB = [Executor = IEB.get()](const clang::driver::Compilation &C) -> llvm::Error {
      llvm::Error E = Executor->UpdateOrcRuntimePathCB(C);
      if (!E)
        return llvm::Error::success();
      const auto &TC = C.getDefaultToolChain();
      // Ubuntu's compiler-rt package uses lib/linux/liborc_rt-<arch>.a,
      // while LLVM 22's discovery can look in a target-triple directory.
      if (TC.getTriple().isOSLinux()) {
        const std::string Arch = TC.getArchName().str();
        for (const std::string &Name : {"liborc_rt-" + Arch + ".a",
                                        "libclang_rt.orc_rt-" + Arch + ".a"}) {
          auto Path = std::filesystem::path(C.getDriver().ResourceDir) / "lib" / "linux" / Name;
          std::error_code EC;
          if (std::filesystem::is_regular_file(Path, EC)) {
            llvm::consumeError(std::move(E));
            Executor->OrcRuntimePath = Path.string();
            return llvm::Error::success();
          }
        }
      }
      return E;
    };
  }
  auto CI = makeCompiler(UserArgs, CB);
  if (!CI)
    return CI.takeError();
  return clang::Interpreter::create(std::move(*CI), std::move(IEB));
}

// ParseAndExecute that reports instead of exiting: the interpreter stays usable
// after a failed input, as in clang-repl.
inline bool run(clang::Interpreter &I, llvm::StringRef Code, clang::Value *V = nullptr) {
  if (llvm::Error E = I.ParseAndExecute(Code, V)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ");
    return false;
  }
  return true;
}

inline std::string valueString(const clang::Value &V) {
  if (!V.isValid())
    return "<invalid Value>\n";
  std::string S;
  llvm::raw_string_ostream OS(S);
  V.print(OS);
  return S;
}

inline std::string maskAddrs(llvm::StringRef S) {
  static const llvm::Regex Hex("0x[0-9a-fA-F]{7,}");
  std::string Out = S.str();
  std::string Err;
  while (Hex.match(Out))
    Out = Hex.sub("0xADDR", Out, &Err);
  return Out;
}

struct Args {
  std::vector<std::string> Positional; // before "--"
  std::vector<std::string> Compiler;   // after "--": passed to the interpreter
};

inline Args splitArgs(int argc, const char **argv) {
  Args A;
  bool After = false;
  for (int I = 1; I < argc; ++I) {
    llvm::StringRef S = argv[I];
    if (!After && S == "--") {
      After = true;
      continue;
    }
    (After ? A.Compiler : A.Positional).push_back(S.str());
  }
  return A;
}

inline std::string readFile(llvm::StringRef Path) {
  auto MB = llvm::MemoryBuffer::getFileOrSTDIN(Path);
  if (!MB) {
    llvm::errs() << "error: cannot read " << Path << ": " << MB.getError().message() << "\n";
    std::exit(1);
  }
  return (*MB)->getBuffer().str();
}

// One input per line; a line ending in a backslash continues the input (the
// backslash is dropped, and for a preprocessor line the newline is kept, as in
// clang-repl's own loop). Blank lines are skipped (clang-repl would turn each
// into an empty input; scripts/session.py skips them too).
inline std::vector<std::string> replInputs(llvm::StringRef Text) {
  std::vector<std::string> Out;
  std::string Cur;
  bool Continuing = false;
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  Text.split(Lines, '\n');
  for (llvm::StringRef L : Lines) {
    L = L.trim();
    if (!Continuing && L.empty())
      continue;
    if (L.ends_with("\\")) {
      Cur += L.drop_back(1).str();
      if (L.starts_with("#"))
        Cur += "\n";
      Continuing = true;
      continue;
    }
    Cur += L.str();
    Out.push_back(Cur);
    Cur.clear();
    Continuing = false;
  }
  if (!Cur.empty())
    Out.push_back(Cur);
  return Out;
}

} // namespace repllab

#endif // REPLLAB_H
