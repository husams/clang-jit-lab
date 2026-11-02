// Part 2.8 -- a minimal Clang plugin: print every top-level declaration.
//
// Build (from the lab root):
//   $LLVM/bin/clang++ -std=c++17 -fno-rtti -fPIC -shared -I$LLVM/include \
//     -undefined dynamic_lookup manifests/p02_plugin.cpp -o out/libprintdecls.dylib
//
// Load into clang-repl with -load / -add-plugin (each passed with --Xcc=-Xclang).
// The plugin's ASTConsumer runs next to the interpreter's own consumers, so it
// sees every input as clang-repl parses it.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace {

class PrintDeclsConsumer : public ASTConsumer {
public:
  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    for (Decl *D : DG) {
      if (auto *ND = dyn_cast<NamedDecl>(D))
        llvm::errs() << "[print-decls] " << D->getDeclKindName() << " '" << ND->getQualifiedNameAsString()
                     << "'\n";
      else
        llvm::errs() << "[print-decls] " << D->getDeclKindName() << "\n";
    }
    return true;
  }
};

class PrintDeclsAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<PrintDeclsConsumer>();
  }
  bool ParseArgs(const CompilerInstance &, const std::vector<std::string> &) override { return true; }
  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<PrintDeclsAction> X("print-decls", "print top-level declarations");
