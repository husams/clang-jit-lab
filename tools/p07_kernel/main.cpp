// Compile one C++ source file, validate its entry signature, call it repeatedly.
#include "repllab.h"
#include "clang/AST/Decl.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Type.h"

#include <charconv>

int main(int argc, const char **argv) {
  repllab::init();
  auto Args = repllab::splitArgs(argc, argv);
  if (Args.Positional.empty()) {
    llvm::errs() << "usage: p07_kernel source.cpp [integer ...] [-- compiler flags]\n";
    return 2;
  }
  std::vector<int> Inputs;
  for (size_t K = 1; K < Args.Positional.size(); ++K) {
    const auto &Text = Args.Positional[K];
    int N = 0;
    auto [End, EC] = std::from_chars(Text.data(), Text.data() + Text.size(), N);
    if (EC != std::errc() || End != Text.data() + Text.size()) {
      llvm::errs() << "invalid integer: " << Text << "\n";
      return 2;
    }
    Inputs.push_back(N);
  }
  if (Inputs.empty())
    Inputs = {0, 6, -3};

  auto Built = repllab::makeInterpreter(Args.Compiler);
  if (!Built) {
    llvm::logAllUnhandledErrors(Built.takeError(), llvm::errs(), "create: ");
    return 1;
  }
  auto Engine = std::move(*Built);
  auto Parsed = Engine->Parse(repllab::readFile(Args.Positional.front()));
  if (!Parsed) {
    llvm::logAllUnhandledErrors(Parsed.takeError(), llvm::errs(), "parse: ");
    return 1;
  }
  clang::FunctionDecl *Entry = nullptr;
  // extern "C" declarations can be nested in a LinkageSpecDecl.
  std::function<void(clang::DeclContext *)> Find = [&](clang::DeclContext *DC) {
    for (clang::Decl *D : DC->decls()) {
      if (auto *Linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(D))
        Find(Linkage);
      if (auto *F = llvm::dyn_cast<clang::FunctionDecl>(D))
        if (F->getNameAsString() == "run" && F->doesThisDeclarationHaveABody())
          Entry = F;
    }
  };
  Find(Parsed->TUPart);
  auto IsInt = [](clang::QualType T) {
    return T.getCanonicalType()->isSpecificBuiltinType(clang::BuiltinType::Int);
  };
  if (!Entry || !Entry->isExternC() || Entry->isVariadic() || Entry->isDeleted() ||
      Entry->getType()->castAs<clang::FunctionProtoType>()->getCallConv() != clang::CC_C ||
      Entry->getNumParams() != 1 || !IsInt(Entry->getReturnType()) ||
      !IsInt(Entry->getParamDecl(0)->getType())) {
    llvm::errs() << "entry must be defined as extern \"C\" int run(int)\n";
    return 1;
  }
  if (llvm::Error E = Engine->Execute(*Parsed)) {
    llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "execute: ");
    return 1;
  }
  auto Address = Engine->getSymbolAddress(clang::GlobalDecl(Entry));
  if (!Address) {
    llvm::logAllUnhandledErrors(Address.takeError(), llvm::errs(), "lookup: ");
    return 1;
  }
  auto Run = Address->toPtr<int (*)(int)>();
  for (int N : Inputs)
    llvm::outs() << "run(" << N << ") = " << Run(N) << "\n";
  return 0;
}
