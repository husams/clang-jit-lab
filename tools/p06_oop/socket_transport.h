#pragma once

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
  Create(llvm::orc::SimpleRemoteEPCTransportClient& Client, int FD) {
    int ShutdownFD = dup(FD);
    if (ShutdownFD < 0) {
      int Error = errno;
      close(FD);
      return llvm::errorCodeToError(std::error_code(Error, std::generic_category()));
    }
    auto Inner = llvm::orc::FDSimpleRemoteEPCTransport::Create(Client, FD);
    if (!Inner) {
      close(ShutdownFD);
      close(FD);
      return Inner.takeError();
    }
    return std::make_unique<SocketTransport>(ShutdownFD, std::move(*Inner));
  }

  SocketTransport(int FD, std::unique_ptr<llvm::orc::FDSimpleRemoteEPCTransport> Transport)
      : ShutdownFD(FD)
      , Inner(std::move(Transport)) {}

  ~SocketTransport() override { disconnect(); }

  llvm::Error start() override { return Inner->start(); }

  llvm::Error sendMessage(llvm::orc::SimpleRemoteEPCOpcode Op, uint64_t Seq,
                          llvm::orc::ExecutorAddr Tag, llvm::ArrayRef<char> Args) override {
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
