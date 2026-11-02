// p06_oop -- out-of-process execution through the API (Section 6.6).
//
//   spawn    IncrementalExecutorBuilder{IsOutOfProcess, OOPExecutor, CustomizeFork}:
//            the interpreter forks llvm-jitlink-executor, JIT code runs there
//   crash [destroy]
//            kill the executor with a segfault in JIT code and see what the
//            host observes
//   connect  start llvm-jitlink-executor listen=127.0.0.1:PORT ourselves and
//            attach with OOPExecutorConnect
//
// Usage: build/bin/p06_oop spawn|crash [destroy]|connect
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/FileSystem.h"
#include "repllab.h"
#if defined(__linux__)
#include "llvm/ExecutionEngine/Orc/SimpleRemoteEPC.h"
#include "socket_transport.h"
#endif

#include <csignal>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace clang;

static const char* ExecutorPath = REPLLAB_LLVM_PREFIX "/bin/llvm-jitlink-executor";
// Absolute, because the child and the doc commands must agree on the file.
static const char* ChildStdout = REPLLAB_LAB_ROOT "/out/p06_child_stdout.txt";

// Ask the kernel for a free localhost port: bind port 0, read the number back,
// release it. (Another process could take it before the executor does; the
// retry loop below fails cleanly in that case instead of attaching to a stranger.)
static int freePort() {
  int S = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in A{};
  A.sin_family = AF_INET;
  A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t Len = sizeof A;
  int Port = 0;
  if (S >= 0 && bind(S, (sockaddr*)&A, sizeof A) == 0 && getsockname(S, (sockaddr*)&A, &Len) == 0) {
    Port = ntohs(A.sin_port);
  }
  if (S >= 0) {
    close(S);
  }
  return Port;
}

static std::string slurp(const char* Path) {
  auto MB = llvm::MemoryBuffer::getFile(Path);
  return MB ? (*MB)->getBuffer().str() : std::string();
}

// The first line of an error, so output does not depend on long messages.
static std::string firstLine(llvm::Error E) {
  std::string S = llvm::toString(std::move(E));
  return S.substr(0, S.find('\n'));
}

// Runs in the forked child, between fork() and exec(): only async-signal-safe
// calls. Send its stdout to a file. (exec() follows, so setvbuf() here would be
// lost: JIT code that prints must fflush(stdout) itself.)
static void redirectChildStdout() {
  int FD = open(ChildStdout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (FD >= 0) {
    dup2(FD, STDOUT_FILENO);
    close(FD);
  }
}

static std::unique_ptr<IncrementalExecutorBuilder> spawnBuilder() {
  auto IEB = std::make_unique<IncrementalExecutorBuilder>();
  IEB->IsOutOfProcess = true;
  IEB->OOPExecutor = ExecutorPath;
  IEB->CustomizeFork = redirectChildStdout;
  return IEB;
}

// Run `printf("child pid: %d\n", getpid())` in the JIT and compare with the
// host. The JIT'd stdout goes to the child's file, so read it back.
static void whoRuns(Interpreter& I, uint32_t ExecutorPID) {
  repllab::run(I, "#include <stdio.h>");
  repllab::run(I, "#include <unistd.h>");
  repllab::run(
      I,
      "printf(\"hello from the JIT\\n\"); printf(\"pid %d\\n\", (int)getpid()); fflush(stdout);");
  std::string Out = slurp(ChildStdout);
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
  llvm::outs() << "JIT pid == ExecutorPID: " << (uint32_t(Pid) == ExecutorPID ? "yes" : "no")
               << "\n";
  llvm::outs() << "JIT pid != host pid:    " << (Pid != getpid() ? "yes" : "no") << "\n";
}

// Note: nothing here reaps the executor of the interpreter we create (the
// interpreter does not either); fine for a one-shot tool, not for a long-lived
// host, which should waitpid(ExecutorPID) after destroying the interpreter.
static int modeSpawn() {
  llvm::ExitOnError ExitOnErr("p06_oop: ");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter({}, spawnBuilder()));
  uint32_t PID = I->getIncrementalExecutorBuilder().ExecutorPID;
  llvm::outs() << "ExecutorPID != host pid: "
               << (PID != 0 && PID != uint32_t(getpid()) ? "yes" : "no") << "\n";
  whoRuns(*I, PID);

  llvm::outs() << "value printing:\n";
  Value V;
  if (llvm::Error E = I->ParseAndExecute("6 * 7", &V)) {
    llvm::outs() << "  error: " << firstLine(std::move(E)) << "\n";
  }
  llvm::outs() << "  V.isValid(): " << (V.isValid() ? "yes" : "no") << "\n";
  llvm::outs() << "plain statements still work:\n";
  repllab::run(*I, "printf(\"after the failed expression\\n\"); fflush(stdout);");
  std::string Out = slurp(ChildStdout);
  size_t At = Out.rfind("after");
  llvm::outs() << "  " << (At == std::string::npos ? "(nothing)\n" : Out.substr(At));
  return 0;
}

static int modeCrash(bool Destroy) {
  llvm::ExitOnError ExitOnErr("p06_oop: ");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter({}, spawnBuilder()));
  uint32_t PID = I->getIncrementalExecutorBuilder().ExecutorPID;
  repllab::run(*I, "#include <stdio.h>");

  llvm::outs() << "input 1: a null write in JIT code\n";
  if (llvm::Error E = I->ParseAndExecute("*(volatile int *)0 = 1;")) {
    llvm::outs() << "  ParseAndExecute -> error: " << firstLine(std::move(E)) << "\n";
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
  llvm::outs() << "host: still running, executor reaped: " << (R == pid_t(PID) ? "yes" : "no")
               << "\n";
  if (R == pid_t(PID) && WIFSIGNALED(Status)) {
    llvm::outs() << "executor died from signal " << WTERMSIG(Status) << " (SIGSEGV is " << SIGSEGV
                 << ")\n";
  }

  llvm::outs() << "input 2: a harmless statement\n";
  if (llvm::Error E = I->ParseAndExecute("printf(\"anyone there?\\n\");")) {
    llvm::outs() << "  ParseAndExecute -> error: " << firstLine(std::move(E)) << "\n";
  } else {
    llvm::outs() << "  ParseAndExecute -> ok\n";
  }

  // ~Interpreter() calls IncrementalExecutor::cleanUp(), which has to talk to
  // the dead executor; the failure is report_fatal_error() (an abort). The
  // default is to leak the interpreter: the process is about to exit anyway.
  if (Destroy) {
    llvm::outs() << "destroying the interpreter\n";
    I.reset();
  } else {
    llvm::outs() << "leaking the interpreter on purpose: its destructor would abort\n";
    (void)I.release();
  }
  return 0;
}

static int modeConnect() {
  llvm::ExitOnError ExitOnErr("p06_oop: ");
  int Port = freePort();
  std::string Addr = "127.0.0.1:" + std::to_string(Port);
  std::string Spec = "listen=" + Addr;

  // Start the executor ourselves and send its stdout to a file.
  pid_t Child = fork();
  if (Child == 0) {
    redirectChildStdout();
    execl(ExecutorPath, ExecutorPath, Spec.c_str(), (char*)nullptr);
    _exit(127);
  }
  llvm::outs() << "executor started on a free localhost port\n";

  // The executor needs a moment before it listens: retry the attach. (A probe
  // connection would use up the executor's single accept(), so the retry is
  // the real attach.)
  llvm::Expected<std::unique_ptr<Interpreter>> IOrErr =
      llvm::make_error<llvm::StringError>("not tried", std::error_code());
  for (int Try = 0; Try < 100; ++Try) {
    llvm::consumeError(IOrErr.takeError());
    auto IEB = std::make_unique<IncrementalExecutorBuilder>();
    IEB->IsOutOfProcess = true;
    IEB->OOPExecutorConnect = Addr; // note: OOPExecutor stays empty
#if defined(__linux__)
    // Use a socket-aware transport on LLVM 22/Linux. The default FD transport
    // closes a descriptor without waking its blocking reader during teardown.
    int FD = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in Endpoint{};
    Endpoint.sin_family = AF_INET;
    Endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Endpoint.sin_port = htons(Port);
    if (FD < 0 || connect(FD, reinterpret_cast<sockaddr*>(&Endpoint), sizeof Endpoint) != 0) {
      int Error = errno;
      if (FD >= 0) {
        close(FD);
      }
      IOrErr = llvm::errorCodeToError(std::error_code(Error, std::generic_category()));
      usleep(20000);
      continue;
    }
    auto Control = llvm::orc::SimpleRemoteEPC::Create<SocketTransport>(
        std::make_unique<llvm::orc::DynamicThreadPoolTaskDispatcher>(std::nullopt),
        llvm::orc::SimpleRemoteEPC::Setup{}, FD);
    if (!Control) {
      IOrErr = Control.takeError();
      break;
    }
    auto Builder = std::make_unique<llvm::orc::LLJITBuilder>();
    Builder->setExecutorProcessControl(std::move(*Control));
    Builder->setPlatformSetUp([Config = IEB.get()](llvm::orc::LLJIT& J) {
      return llvm::orc::ExecutorNativePlatform(Config->OrcRuntimePath)(J);
    });
    IEB->ExecutorPID = uint32_t(-1); // connecting does not tell us the remote PID
    IEB->JITBuilder = std::move(Builder);
#endif
    IOrErr = repllab::makeInterpreter({}, std::move(IEB));
    if (IOrErr) {
      break;
    }
    usleep(20000);
  }
  if (!IOrErr) {
    llvm::outs() << "attach failed: " << firstLine(IOrErr.takeError()) << "\n";
    kill(Child, SIGKILL);
    waitpid(Child, nullptr, 0);
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*IOrErr);
  llvm::outs() << "attached via OOPExecutorConnect\n";
  // No fork happened; the builder leaves ExecutorPID at (uint32_t)-1.
  llvm::outs() << "ExecutorPID is (uint32_t)-1: "
               << (I->getIncrementalExecutorBuilder().ExecutorPID == uint32_t(-1) ? "yes" : "no")
               << "\n";
  repllab::run(*I, "#include <stdio.h>");
  repllab::run(*I, "#include <unistd.h>");
  repllab::run(
      *I, "printf(\"hello over TCP\\n\"); printf(\"pid %d\\n\", (int)getpid()); fflush(stdout);");
  std::string Out = slurp(ChildStdout);
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
  llvm::outs() << "JIT pid == the executor we started: " << (Pid == Child ? "yes" : "no") << "\n";
  llvm::outs() << "JIT pid != host pid:                " << (Pid != getpid() ? "yes" : "no")
               << "\n";

  I.reset(); // cleanUp() over the socket, then the connection closes
  int Status = 0;
  // The executor exits when its client disconnects; if not, make sure it does.
  if (waitpid(Child, &Status, WNOHANG) == 0) {
    kill(Child, SIGTERM);
    waitpid(Child, &Status, 0);
  }
  return 0;
}

int main(int argc, const char** argv) {
  repllab::init();
  llvm::sys::fs::create_directories(REPLLAB_LAB_ROOT "/out");
  llvm::StringRef Mode = argc > 1 ? argv[1] : "";
  if (Mode == "spawn") {
    return modeSpawn();
  }
  if (Mode == "crash") {
    return modeCrash(argc > 2 && llvm::StringRef(argv[2]) == "destroy");
  }
  if (Mode == "connect") {
    return modeConnect();
  }
  llvm::errs() << "usage: p06_oop spawn|crash|connect\n";
  return 2;
}
