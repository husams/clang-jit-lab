#include <cerrno>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/ToolChain.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <memory>

#include <sys/socket.h>
#include <unistd.h>

#include "llvm/ExecutionEngine/Orc/Shared/SimpleRemoteEPCUtils.h"

// LLVM 22 closes the socket while another thread is blocked in read(). Linux
// requires shutdown() to wake that reader before SimpleRemoteEPC waits for it.
// Compose the public transport API; keep a duplicate solely for shutdown so a
// descriptor closed internally by LLVM cannot be reused underneath this code.
class SocketTransport final : public llvm::orc::SimpleRemoteEPCTransport {
public:
  static llvm::Expected<std::unique_ptr<SocketTransport>>
  Create(llvm::orc::SimpleRemoteEPCTransportClient &Client, int FD) {
    int ShutdownFD = dup(FD);
    if (ShutdownFD < 0) {
      int Error = errno;
      close(FD);
      return llvm::errorCodeToError(
          std::error_code(Error, std::generic_category()));
    }
    // Wrap LLVM's FD transport while retaining a descriptor that can wake the
    // Linux reader.
    auto Inner = llvm::orc::FDSimpleRemoteEPCTransport::Create(Client, FD);
    if (!Inner) {
      close(ShutdownFD);
      close(FD);
      return Inner.takeError();
    }
    return std::make_unique<SocketTransport>(ShutdownFD, std::move(*Inner));
  }

  SocketTransport(
      int FD, std::unique_ptr<llvm::orc::FDSimpleRemoteEPCTransport> Transport)
      : ShutdownFD(FD), Inner(std::move(Transport)) {}

  ~SocketTransport() override { disconnect(); }

  llvm::Error start() override { return Inner->start(); }

  llvm::Error sendMessage(llvm::orc::SimpleRemoteEPCOpcode Op, uint64_t Seq,
                          llvm::orc::ExecutorAddr Tag,
                          llvm::ArrayRef<char> Args) override {
    return Inner->sendMessage(Op, Seq, Tag, Args);
  }

  void disconnect() override {
    if (!Disconnected.exchange(true)) {
      shutdown(ShutdownFD, SHUT_RDWR);
      close(ShutdownFD);
      Inner->disconnect();
    }
  }

private:
  int ShutdownFD;
  std::unique_ptr<llvm::orc::FDSimpleRemoteEPCTransport> Inner;
  std::atomic<bool> Disconnected{false};
};

#endif

#if defined(__linux__)
#include "llvm/ExecutionEngine/Orc/SimpleRemoteEPC.h"
#endif

using namespace clang;

static std::string ExecutorPath;
// Absolute, because the child and the doc commands must agree on the file.
static const char *ChildStdout = "executor_stdout.txt";

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Executor demonstration: spawn, crash or connect"),
      llvm::cl::cat(Options), llvm::cl::Required);
  llvm::cl::opt<bool> Destroy(
      "destroy",
      llvm::cl::desc("Destroy the engine after the crash demonstration"),
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
  const char *Prefix = std::getenv("LLVM");
  if (!Prefix) {
    llvm::errs() << "Set LLVM to the LLVM 22 installation prefix.\n";
    return 2;
  }
  ExecutorPath = std::string(Prefix) + "/bin/llvm-jitlink-executor";
  if (Mode == "spawn") {
    {

      auto ExecutorOptions = std::make_unique<IncrementalExecutorBuilder>();
      // Keep Clang in the host and place generated code in an executor with a
      // separate address space.
      ExecutorOptions->IsOutOfProcess = true;
      ExecutorOptions->OOPExecutor = ExecutorPath;
      // This callback runs between fork and exec: restrict it to
      // async-signal-safe operations.
      ExecutorOptions->CustomizeFork = [] {
        int FD = open(ChildStdout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (FD >= 0) {
          dup2(FD, STDOUT_FILENO);
          close(FD);
        }
      };

      std::vector<const char *> CompilerArguments;
      for (const std::string &Flag : (CompilerFlags)) {
        CompilerArguments.push_back(Flag.c_str());
      }
      clang::IncrementalCompilerBuilder CompilerBuilder;
      // Configure the embedded compiler, independently of the host build flags.
      // These argument strings must remain alive until CreateCpp() finishes.
      CompilerBuilder.SetCompilerArgs(CompilerArguments);
      if (ExecutorOptions && ExecutorOptions->IsOutOfProcess &&
          ExecutorOptions->OrcRuntimePath.empty()) {
        // Inspect Clang's selected toolchain while building its invocation to
        // locate the ORC runtime.
        CompilerBuilder.SetDriverCompilationCallback(
            [Executor = ExecutorOptions.get()](
                const clang::driver::Compilation &C) -> llvm::Error {
              llvm::Error E = Executor->UpdateOrcRuntimePathCB(C);
              if (!E) {
                return llvm::Error::success();
              }
              const auto &TC = C.getDefaultToolChain();
              // Ubuntu's compiler-rt package uses lib/linux/liborc_rt-<arch>.a,
              // while LLVM 22's discovery can look in a target-triple
              // directory.
              if (TC.getTriple().isOSLinux()) {
                const std::string Arch = TC.getArchName().str();
                for (const std::string &Name :
                     {"liborc_rt-" + Arch + ".a",
                      "libclang_rt.orc_rt-" + Arch + ".a"}) {
                  auto Path = std::filesystem::path(C.getDriver().ResourceDir) /
                              "lib" / "linux" / Name;
                  std::error_code EC;
                  if (std::filesystem::is_regular_file(Path, EC)) {
                    llvm::consumeError(std::move(E));
                    Executor->OrcRuntimePath = Path.string();
                    return llvm::Error::success();
                  }
                }
              }
              return E;
            });
      }
      // CreateCpp returns an owned CompilerInstance or an error inside
      // Expected.
      auto CompilerOrError = CompilerBuilder.CreateCpp();
      // Extract the failed Expected with takeError(); logging consumes that
      // Error.
      if (!CompilerOrError) {
        llvm::logAllUnhandledErrors(CompilerOrError.takeError(), llvm::errs(),
                                    "compiler: ");
        return 1;
      }

      // Give the session the compiler and prepare its runtime prelude.
      auto InterpreterOrError = clang::Interpreter::create(
          std::move(*CompilerOrError), std::move(ExecutorOptions));
      if (!InterpreterOrError) {
        llvm::logAllUnhandledErrors(InterpreterOrError.takeError(),
                                    llvm::errs(), "error: ");
        return 1;
      }
      std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);
      uint32_t PID = I->getIncrementalExecutorBuilder().ExecutorPID;
      llvm::outs() << "ExecutorPID != host pid: "
                   << (PID != 0 && PID != uint32_t(getpid()) ? "yes" : "no")
                   << "\n";
      {
        auto &&IArgument = (*I);
        auto &&ExecutorPIDArgument = (PID);

        // Compile this input, then submit its module and run its initializers.
        if (llvm::Error Error = (IArgument).ParseAndExecute(R"cpp(
#include <stdio.h>
)cpp")) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
        if (llvm::Error Error = (IArgument).ParseAndExecute(R"cpp(
#include <unistd.h>
)cpp")) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
        if (llvm::Error Error = (IArgument).ParseAndExecute(
                R"cpp(
printf("hello from the JIT\n");
printf("pid %d\n", (int)getpid());
fflush(stdout);
)cpp")) {
          llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(),
                                      "error: ");
        }
        std::string ExecutorOutput;
        auto Buffer = llvm::MemoryBuffer::getFile((ChildStdout));
        ExecutorOutput = Buffer ? (*Buffer)->getBuffer().str() : std::string();
        std::string Out = ExecutorOutput;
        llvm::outs() << "child stdout file:\n";
        int Pid = 0;
        llvm::SmallVector<llvm::StringRef, 4> Lines;
        llvm::StringRef(Out).split(Lines, '\n', -1, false);
        for (llvm::StringRef L : Lines) {
          if (L.starts_with("pid ")) {
            L.drop_front(4).getAsInteger(10, Pid);
            llvm::outs() << "  pid <number>\n";
          } else {
            llvm::outs() << "  " << L << "\n";
          }
        }
        llvm::outs() << "JIT pid == ExecutorPID: "
                     << (uint32_t(Pid) == ExecutorPIDArgument ? "yes" : "no")
                     << "\n";
        llvm::outs() << "JIT pid != host pid:    "
                     << (Pid != getpid() ? "yes" : "no") << "\n";
      }

      llvm::outs() << "value printing:\n";
      Value V;
      if (llvm::Error E = I->ParseAndExecute(R"cpp(
6 * 7
)cpp",
                                             &V)) {
        llvm::outs() << "  error: " << (llvm::toString((std::move(E)))) << "\n";
      }
      llvm::outs() << "  V.isValid(): " << (V.isValid() ? "yes" : "no") << "\n";
      llvm::outs() << "plain statements still work:\n";
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(
printf("after the failed expression\n");
fflush(stdout);
)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      std::string ExecutorOutput2;
      auto Buffer = llvm::MemoryBuffer::getFile((ChildStdout));
      ExecutorOutput2 = Buffer ? (*Buffer)->getBuffer().str() : std::string();
      std::string Out = ExecutorOutput2;
      size_t At = Out.rfind("after");
      llvm::outs() << "  "
                   << (At == std::string::npos ? "(nothing)\n"
                                               : Out.substr(At));
      return 0;
    }
  }
  if (Mode == "crash") {
    {

      auto ExecutorOptions2 = std::make_unique<IncrementalExecutorBuilder>();
      ExecutorOptions2->IsOutOfProcess = true;
      ExecutorOptions2->OOPExecutor = ExecutorPath;
      ExecutorOptions2->CustomizeFork = [] {
        int FD = open(ChildStdout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (FD >= 0) {
          dup2(FD, STDOUT_FILENO);
          close(FD);
        }
      };

      std::vector<const char *> CompilerArguments2;
      for (const std::string &Flag : (CompilerFlags)) {
        CompilerArguments2.push_back(Flag.c_str());
      }
      clang::IncrementalCompilerBuilder CompilerBuilder2;
      CompilerBuilder2.SetCompilerArgs(CompilerArguments2);
      if (ExecutorOptions2 && ExecutorOptions2->IsOutOfProcess &&
          ExecutorOptions2->OrcRuntimePath.empty()) {
        CompilerBuilder2.SetDriverCompilationCallback(
            [Executor = ExecutorOptions2.get()](
                const clang::driver::Compilation &C) -> llvm::Error {
              llvm::Error E = Executor->UpdateOrcRuntimePathCB(C);
              if (!E) {
                return llvm::Error::success();
              }
              const auto &TC = C.getDefaultToolChain();
              // Ubuntu's compiler-rt package uses lib/linux/liborc_rt-<arch>.a,
              // while LLVM 22's discovery can look in a target-triple
              // directory.
              if (TC.getTriple().isOSLinux()) {
                const std::string Arch = TC.getArchName().str();
                for (const std::string &Name :
                     {"liborc_rt-" + Arch + ".a",
                      "libclang_rt.orc_rt-" + Arch + ".a"}) {
                  auto Path = std::filesystem::path(C.getDriver().ResourceDir) /
                              "lib" / "linux" / Name;
                  std::error_code EC;
                  if (std::filesystem::is_regular_file(Path, EC)) {
                    llvm::consumeError(std::move(E));
                    Executor->OrcRuntimePath = Path.string();
                    return llvm::Error::success();
                  }
                }
              }
              return E;
            });
      }
      auto CompilerOrError2 = CompilerBuilder2.CreateCpp();
      if (!CompilerOrError2) {
        llvm::logAllUnhandledErrors(CompilerOrError2.takeError(), llvm::errs(),
                                    "compiler: ");
        return 1;
      }

      auto InterpreterOrError2 = clang::Interpreter::create(
          std::move(*CompilerOrError2), std::move(ExecutorOptions2));
      if (!InterpreterOrError2) {
        llvm::logAllUnhandledErrors(InterpreterOrError2.takeError(),
                                    llvm::errs(), "error: ");
        return 1;
      }
      std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError2);
      uint32_t PID = I->getIncrementalExecutorBuilder().ExecutorPID;
      if (llvm::Error Error = I->ParseAndExecute(R"cpp(
#include <stdio.h>
)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }

      llvm::outs() << "input 1: a null write in JIT code\n";
      if (llvm::Error E = I->ParseAndExecute(R"cpp(
*(volatile int *)0 = 1;
)cpp")) {
        llvm::outs() << "  ParseAndExecute -> error: "
                     << (llvm::toString((std::move(E)))) << "\n";
      }

      // Bounded wait: if the executor somehow survived, do not block forever.
      int Status = 0;
      pid_t R = 0;
      for (int Try = 0; Try < 100 && R == 0; ++Try) {
        R = waitpid(PID, &Status, WNOHANG);
        if (R == 0) {
          usleep(20000);
        }
      }
      if (R == 0) {
        kill(PID, SIGKILL);
        R = waitpid(PID, &Status, 0);
      }
      llvm::outs() << "host: still running, executor reaped: "
                   << (R == pid_t(PID) ? "yes" : "no") << "\n";
      if (R == pid_t(PID) && WIFSIGNALED(Status)) {
        llvm::outs() << "executor died from signal " << WTERMSIG(Status)
                     << " (SIGSEGV is " << SIGSEGV << ")\n";
      }

      llvm::outs() << "input 2: a harmless statement\n";
      if (llvm::Error E = I->ParseAndExecute(R"cpp(
printf("anyone there?\n");
)cpp")) {
        llvm::outs() << "  ParseAndExecute -> error: "
                     << (llvm::toString((std::move(E)))) << "\n";
      } else {
        llvm::outs() << "  ParseAndExecute -> ok\n";
      }

      // ~Interpreter() calls IncrementalExecutor::cleanUp(), which has to talk
      // to the dead executor; the failure is report_fatal_error() (an abort).
      // The default is to leak the interpreter: the process is about to exit
      // anyway.
      if (Destroy) {
        llvm::outs() << "destroying the interpreter\n";
        I.reset();
      } else {
        llvm::outs() << "leaking the interpreter on purpose: its destructor "
                        "would abort\n";
        (void)I.release();
      }
      return 0;
    }
  }
  if (Mode == "connect") {
    {

      int PortNumber = 0;

      int S = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in A{};
      A.sin_family = AF_INET;
      A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      socklen_t Len = sizeof A;

      if (S >= 0 && bind(S, (sockaddr *)&A, sizeof A) == 0 &&
          getsockname(S, (sockaddr *)&A, &Len) == 0) {
        PortNumber = ntohs(A.sin_port);
      }
      if (S >= 0) {
        close(S);
      }

      int Port = PortNumber;
      std::string Addr = "127.0.0.1:" + std::to_string(Port);
      std::string Spec = "listen=" + Addr;

      // Start the executor ourselves and send its stdout to a file.
      pid_t Child = fork();
      if (Child == 0) {
        {

          int FD = open(ChildStdout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (FD >= 0) {
            dup2(FD, STDOUT_FILENO);
            close(FD);
          }
        }
        execl(ExecutorPath.c_str(), ExecutorPath.c_str(), Spec.c_str(),
              (char *)nullptr);
        _exit(127);
      }
      llvm::outs() << "executor started on a free localhost port\n";

      // The executor needs a moment before it listens: retry the attach. (A
      // probe connection would use up the executor's single accept(), so the
      // retry is the real attach.)
      llvm::Expected<std::unique_ptr<Interpreter>> IOrErr =
          llvm::make_error<llvm::StringError>("not tried", std::error_code());
      for (int Try = 0; Try < 100; ++Try) {
        llvm::consumeError(IOrErr.takeError());
        auto IEB = std::make_unique<IncrementalExecutorBuilder>();
        IEB->IsOutOfProcess = true;
        // Attach to the listening executor instead of spawning it; its
        // addresses remain remote.
        IEB->OOPExecutorConnect = Addr; // note: OOPExecutor stays empty
        // Validate the compiler before opening the remote transport.
        std::vector<const char *> CompilerArguments3;
        for (const std::string &Flag : (CompilerFlags)) {
          CompilerArguments3.push_back(Flag.c_str());
        }
        clang::IncrementalCompilerBuilder CompilerBuilder3;
        CompilerBuilder3.SetCompilerArgs(CompilerArguments3);
        if (IEB && IEB->IsOutOfProcess && IEB->OrcRuntimePath.empty()) {
          CompilerBuilder3.SetDriverCompilationCallback(
              [Executor = IEB.get()](
                  const clang::driver::Compilation &C) -> llvm::Error {
                llvm::Error E = Executor->UpdateOrcRuntimePathCB(C);
                if (!E) {
                  return llvm::Error::success();
                }
                const auto &TC = C.getDefaultToolChain();
                // Ubuntu's compiler-rt package uses
                // lib/linux/liborc_rt-<arch>.a, while LLVM 22's discovery can
                // look in a target-triple directory.
                if (TC.getTriple().isOSLinux()) {
                  const std::string Arch = TC.getArchName().str();
                  for (const std::string &Name :
                       {"liborc_rt-" + Arch + ".a",
                        "libclang_rt.orc_rt-" + Arch + ".a"}) {
                    auto Path =
                        std::filesystem::path(C.getDriver().ResourceDir) /
                        "lib" / "linux" / Name;
                    std::error_code EC;
                    if (std::filesystem::is_regular_file(Path, EC)) {
                      llvm::consumeError(std::move(E));
                      Executor->OrcRuntimePath = Path.string();
                      return llvm::Error::success();
                    }
                  }
                }
                return E;
              });
        }
        auto CompilerOrError3 = CompilerBuilder3.CreateCpp();
        if (!CompilerOrError3) {
          IOrErr = CompilerOrError3.takeError();
          break;
        }

#if defined(__linux__)
        // Use a socket-aware transport on LLVM 22/Linux. The default FD
        // transport closes a descriptor without waking its blocking reader
        // during teardown.
        int FD = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in Endpoint{};
        Endpoint.sin_family = AF_INET;
        Endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Endpoint.sin_port = htons(Port);
        if (FD < 0 || connect(FD, reinterpret_cast<sockaddr *>(&Endpoint),
                              sizeof Endpoint) != 0) {
          int Error = errno;
          if (FD >= 0) {
            close(FD);
          }
          IOrErr = llvm::errorCodeToError(
              std::error_code(Error, std::generic_category()));
          usleep(20000);
          continue;
        }
        auto Control = llvm::orc::SimpleRemoteEPC::Create<SocketTransport>(
            std::make_unique<llvm::orc::DynamicThreadPoolTaskDispatcher>(
                std::nullopt),
            llvm::orc::SimpleRemoteEPC::Setup{}, FD);
        if (!Control) {
          IOrErr = Control.takeError();
          break;
        }
        auto Builder = std::make_unique<llvm::orc::LLJITBuilder>();
        Builder->setExecutorProcessControl(std::move(*Control));
        Builder->setPlatformSetUp([Config = IEB.get()](llvm::orc::LLJIT &J) {
          return llvm::orc::ExecutorNativePlatform(Config->OrcRuntimePath)(J);
        });
        IEB->ExecutorPID =
            uint32_t(-1); // connecting does not tell us the remote PID
        IEB->JITBuilder = std::move(Builder);
#endif
        IOrErr = clang::Interpreter::create(std::move(*CompilerOrError3),
                                            std::move(IEB));
        if (IOrErr) {
          break;
        }
        usleep(20000);
      }
      if (!IOrErr) {
        llvm::outs() << "attach failed: "
                     << (llvm::toString((IOrErr.takeError()))) << "\n";
        kill(Child, SIGKILL);
        waitpid(Child, nullptr, 0);
        return 1;
      }
      std::unique_ptr<Interpreter> I = std::move(*IOrErr);
      llvm::outs() << "attached via OOPExecutorConnect\n";
      // No fork happened; the builder leaves ExecutorPID at (uint32_t)-1.
      llvm::outs() << "ExecutorPID is (uint32_t)-1: "
                   << (I->getIncrementalExecutorBuilder().ExecutorPID ==
                               uint32_t(-1)
                           ? "yes"
                           : "no")
                   << "\n";
      if (llvm::Error Error = I->ParseAndExecute(R"cpp(
#include <stdio.h>
)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      if (llvm::Error Error = I->ParseAndExecute(R"cpp(
#include <unistd.h>
)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      if (llvm::Error Error = I->ParseAndExecute(
              R"cpp(
printf("hello over TCP\n");
printf("pid %d\n", (int)getpid());
fflush(stdout);
)cpp")) {
        llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      }
      std::string ExecutorOutput3;
      auto Buffer = llvm::MemoryBuffer::getFile((ChildStdout));
      ExecutorOutput3 = Buffer ? (*Buffer)->getBuffer().str() : std::string();
      std::string Out = ExecutorOutput3;
      int Pid = 0;
      llvm::SmallVector<llvm::StringRef, 4> Lines;
      llvm::StringRef(Out).split(Lines, '\n', -1, false);
      llvm::outs() << "remote executor stdout file:\n";
      for (llvm::StringRef L : Lines) {
        if (L.starts_with("pid ")) {
          L.drop_front(4).getAsInteger(10, Pid);
          llvm::outs() << "  pid <number>\n";
        } else {
          llvm::outs() << "  " << L << "\n";
        }
      }
      llvm::outs() << "JIT pid == the executor we started: "
                   << (Pid == Child ? "yes" : "no") << "\n";
      llvm::outs() << "JIT pid != host pid:                "
                   << (Pid != getpid() ? "yes" : "no") << "\n";

      I.reset(); // cleanUp() over the socket, then the connection closes
      int Status = 0;
      // The executor exits when its client disconnects; if not, make sure it
      // does.
      if (waitpid(Child, &Status, WNOHANG) == 0) {
        kill(Child, SIGTERM);
        waitpid(Child, &Status, 0);
      }
      return 0;
    }
  }
  llvm::errs() << "usage: p06_oop --mode=spawn|crash|connect\n";
  return 2;
}
