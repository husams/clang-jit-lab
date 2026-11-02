#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// ---- 5.8: layout of one record --------------------------------------------

struct Region {
  uint64_t Bit, Bits;
  std::string What;
};

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <cstddef>
)cpp",
    R"cpp(
#include <string>
)cpp",
    R"cpp(
using Id = long;
)cpp",
    R"cpp(
typedef const Id CId;
)cpp",
    R"cpp(
CId answer = 42;
)cpp",
    R"cpp(
const CId *pid = &answer;
)cpp",
    R"cpp(
std::size_t count = 3;
)cpp",
    R"cpp(
std::string text = "layout";
)cpp",
    R"cpp(
auto deduced = pid;
)cpp",
    R"cpp(
struct Inner {
  char c;
  double d;
};
)cpp",
    R"cpp(
struct Base {
  int b;
};
)cpp",
    R"cpp(
struct Layout : Base {
  char tag;
  short s;
  Inner in;
  int arr[3];
  bool flag;
  void *p;
};
)cpp",
    R"cpp(
struct Poly {
  virtual ~Poly() {
  }
  int v;
};
)cpp",
    R"cpp(
struct Bits {
  unsigned a : 3;
  unsigned b : 5;
  int c;
};
)cpp",
    R"cpp(
struct VB {
  int v;
};
)cpp",
    R"cpp(
struct VD : virtual VB {
  int d;
};
)cpp",
    R"cpp(
union U {
  char c;
  double d;
  int i[3];
};
)cpp",
    R"cpp(
template <typename T> struct Pair {
  T first;
  char tag;
  T second;
};
)cpp",
    R"cpp(
Pair<int> pi;
)cpp",
    R"cpp(
Pair<double> pd;
)cpp",
    R"cpp(
Pair<char> pc;
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::opt<std::string> Mode(
      "mode", llvm::cl::desc("Demonstration: types, layout, template"),
      llvm::cl::cat(Options), llvm::cl::init("layout"));
  llvm::cl::list<std::string> NamesOption(
      "name", llvm::cl::ZeroOrMore,
      llvm::cl::desc("Name to inspect; repeat to inspect several"),
      llvm::cl::cat(Options));
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

  std::vector<std::string> Names(NamesOption.begin(), NamesOption.end());
  if (Names.empty()) {
    Names = {"Inner", "Layout"};
  }
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
      clang::Interpreter::create(std::move(*CompilerOrError), nullptr);
  if (!InterpreterOrError) {
    llvm::logAllUnhandledErrors(InterpreterOrError.takeError(), llvm::errs(),
                                "error: ");
    return 1;
  }
  std::unique_ptr<Interpreter> I = std::move(*InterpreterOrError);
  for (const std::string &Code : SourceInputs) {
    // Compile this input, then submit its module and run its initializers.
    if (llvm::Error Error = I->ParseAndExecute(Code)) {
      llvm::logAllUnhandledErrors(std::move(Error), llvm::errs(), "error: ");
      return 1;
    }
  }
  Sema &S = I->getCompilerInstance()->getSema();
  ASTContext &C = S.getASTContext();

  for (size_t K = 0; K < Names.size(); ++K) {
    clang::NamedDecl *FoundDeclaration;
    clang::ASTContext &LookupContext = S.getASTContext();
    clang::LookupResult Lookup(
        (S), clang::DeclarationName(&LookupContext.Idents.get((Names[K]))),
        clang::SourceLocation(), clang::Sema::LookupOrdinaryName);
    S.LookupName(Lookup, S.TUScope);
    FoundDeclaration =
        Lookup.empty() ? nullptr : Lookup.getRepresentativeDecl();
    NamedDecl *ND = FoundDeclaration;
    if (!ND) {
      llvm::outs() << Names[K] << ": not found\n";
      continue;
    }
    if (Mode == "types") {
      auto *VD = llvm::dyn_cast<VarDecl>(ND);
      if (!VD) {
        llvm::outs() << Names[K] << ": not a variable\n";
        continue;
      }
      // Keep readable spelling for display; canonical types remove aliases for
      // semantic comparison.
      QualType T = VD->getType();
      llvm::outs() << Names[K] << ":\n";
      llvm::outs() << "  as written          "
                   << (T.getAsString(C.getPrintingPolicy())) << "\n";
      llvm::outs() << "  one step desugared  "
                   << ((T.getSingleStepDesugaredType(C))
                           .getAsString(C.getPrintingPolicy()))
                   << "\n";
      llvm::outs()
          << "  desugared           "
          << ((T.getDesugaredType(C)).getAsString(C.getPrintingPolicy()))
          << "\n";
      llvm::outs()
          << "  canonical           "
          << ((T.getCanonicalType()).getAsString(C.getPrintingPolicy()))
          << "\n";
      llvm::outs() << "  class of the type   " << T->getTypeClassName()
                   << " -> " << T.getCanonicalType()->getTypeClassName()
                   << "\n";
    } else if (Mode == "layout") {
      auto *RD = llvm::dyn_cast<CXXRecordDecl>(ND);
      if (!RD || !RD->getDefinition()) {
        llvm::outs() << Names[K] << ": not a complete class\n";
        continue;
      }
      {
        clang::ASTContext &CArgument = (C);
        const clang::CXXRecordDecl *RDArgument = (RD);

        RDArgument = RDArgument->getDefinition();
        // Ask the current target's ASTContext for a complete record's actual
        // storage layout.
        const ASTRecordLayout &L = CArgument.getASTRecordLayout(RDArgument);
        llvm::outs() << (RDArgument->isUnion() ? "union " : "struct ")
                     << (clang::QualType(
                             CArgument.getCanonicalTagType(RDArgument))
                             .getAsString((CArgument).getPrintingPolicy()))
                     << ": size " << L.getSize().getQuantity() << ", align "
                     << L.getAlignment().getQuantity() << ", data size "
                     << L.getDataSize().getQuantity() << ", non-virtual size "
                     << L.getNonVirtualSize().getQuantity() << "\n";

        std::vector<Region> Regions;
        if (RDArgument->isDynamicClass() && L.hasOwnVFPtr()) {
          Regions.push_back(
              {0, CArgument.getTypeSize(CArgument.VoidPtrTy), "<vptr>"});
        }
        for (const CXXBaseSpecifier &B : RDArgument->bases()) {
          const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
          // A base's data size, not its size: the derived class may reuse its
          // tail padding. A virtual base lives wherever the most-derived class
          // puts it.
          CharUnits BOff = B.isVirtual() ? L.getVBaseClassOffset(BRD)
                                         : L.getBaseClassOffset(BRD);
          Regions.push_back(
              {(uint64_t)BOff.getQuantity() * 8,
               (uint64_t)CArgument.getASTRecordLayout(BRD)
                       .getDataSize()
                       .getQuantity() *
                   8,
               std::string(B.isVirtual() ? "virtual base " : "base ") +
                   BRD->getNameAsString()});
        }
        unsigned I = 0;
        for (const FieldDecl *F : RDArgument->fields()) {
          uint64_t Bits = F->isBitField() ? F->getBitWidthValue()
                                          : CArgument.getTypeSize(F->getType());
          std::string What =
              F->getNameAsString() + " : " +
              ((F->getType()).getAsString((CArgument).getPrintingPolicy()));
          if (F->isBitField()) {
            What += " : " + std::to_string(Bits);
          }
          // Field offsets are bits, unlike CharUnits sizes and base offsets;
          // retain those units here.
          Regions.push_back({L.getFieldOffset(I++), Bits, What});
        }
        std::stable_sort(
            Regions.begin(), Regions.end(),
            [](const Region &A, const Region &B) { return A.Bit < B.Bit; });

        llvm::outs() << "  offset  bits  what\n";
        uint64_t Cursor = 0;

        for (const Region &R : Regions) {
          if (R.Bit > Cursor) {
            {
              auto &&BitArgument = (Cursor);
              auto &&BitsArgument = (R.Bit - Cursor);
              auto &&WhatArgument = ("<padding>");

              llvm::outs() << llvm::format("  %6s  %4llu  ",
                                           (std::to_string((BitArgument) / 8) +
                                            (((BitArgument) % 8)
                                                 ? "." + std::to_string(
                                                             (BitArgument) % 8)
                                                 : ""))
                                               .c_str(),
                                           (unsigned long long)BitsArgument)
                           << WhatArgument << "\n";
            }
          }
          {
            auto &&BitArgument2 = (R.Bit);
            auto &&BitsArgument2 = (R.Bits);
            auto &&WhatArgument2 = (R.What);

            llvm::outs() << llvm::format(
                                "  %6s  %4llu  ",
                                (std::to_string((BitArgument2) / 8) +
                                 (((BitArgument2) % 8)
                                      ? "." + std::to_string((BitArgument2) % 8)
                                      : ""))
                                    .c_str(),
                                (unsigned long long)BitsArgument2)
                         << WhatArgument2 << "\n";
          }
          Cursor =
              std::max(Cursor,
                       R.Bit + R.Bits); // unions overlap: keep the furthest end
        }
        uint64_t End = (uint64_t)L.getSize().getQuantity() * 8;
        if (Cursor < End) {
          {
            auto &&BitArgument3 = (Cursor);
            auto &&BitsArgument3 = (End - Cursor);
            auto &&WhatArgument3 = ("<tail padding>");

            llvm::outs() << llvm::format(
                                "  %6s  %4llu  ",
                                (std::to_string((BitArgument3) / 8) +
                                 (((BitArgument3) % 8)
                                      ? "." + std::to_string((BitArgument3) % 8)
                                      : ""))
                                    .c_str(),
                                (unsigned long long)BitsArgument3)
                         << WhatArgument3 << "\n";
          }
        }
      }
    } else if (Mode == "template") {
      auto *CT = llvm::dyn_cast<ClassTemplateDecl>(ND);
      if (!CT) {
        llvm::outs() << Names[K] << ": not a class template\n";
        continue;
      }
      std::vector<ClassTemplateSpecializationDecl *> Specs(
          CT->specializations().begin(), CT->specializations().end());
      std::sort(Specs.begin(), Specs.end(), [&](auto *X, auto *Y) {
        return (clang::QualType(C.getCanonicalTagType(X))
                    .getAsString(C.getPrintingPolicy())) <
               (clang::QualType(C.getCanonicalTagType(Y))
                    .getAsString(C.getPrintingPolicy()));
      });
      for (ClassTemplateSpecializationDecl *Spec : Specs) {
        if (Spec->isCompleteDefinition()) {
          {
            clang::ASTContext &CArgument2 = (C);
            const clang::CXXRecordDecl *RDArgument2 = (Spec);

            RDArgument2 = RDArgument2->getDefinition();
            const ASTRecordLayout &L =
                CArgument2.getASTRecordLayout(RDArgument2);
            llvm::outs() << (RDArgument2->isUnion() ? "union " : "struct ")
                         << (clang::QualType(
                                 CArgument2.getCanonicalTagType(RDArgument2))
                                 .getAsString((CArgument2).getPrintingPolicy()))
                         << ": size " << L.getSize().getQuantity() << ", align "
                         << L.getAlignment().getQuantity() << ", data size "
                         << L.getDataSize().getQuantity()
                         << ", non-virtual size "
                         << L.getNonVirtualSize().getQuantity() << "\n";

            std::vector<Region> Regions;
            if (RDArgument2->isDynamicClass() && L.hasOwnVFPtr()) {
              Regions.push_back(
                  {0, CArgument2.getTypeSize(CArgument2.VoidPtrTy), "<vptr>"});
            }
            for (const CXXBaseSpecifier &B : RDArgument2->bases()) {
              const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
              // A base's data size, not its size: the derived class may reuse
              // its tail padding. A virtual base lives wherever the
              // most-derived class puts it.
              CharUnits BOff = B.isVirtual() ? L.getVBaseClassOffset(BRD)
                                             : L.getBaseClassOffset(BRD);
              Regions.push_back(
                  {(uint64_t)BOff.getQuantity() * 8,
                   (uint64_t)CArgument2.getASTRecordLayout(BRD)
                           .getDataSize()
                           .getQuantity() *
                       8,
                   std::string(B.isVirtual() ? "virtual base " : "base ") +
                       BRD->getNameAsString()});
            }
            unsigned I = 0;
            for (const FieldDecl *F : RDArgument2->fields()) {
              uint64_t Bits = F->isBitField()
                                  ? F->getBitWidthValue()
                                  : CArgument2.getTypeSize(F->getType());
              std::string What =
                  F->getNameAsString() + " : " +
                  ((F->getType())
                       .getAsString((CArgument2).getPrintingPolicy()));
              if (F->isBitField()) {
                What += " : " + std::to_string(Bits);
              }
              Regions.push_back({L.getFieldOffset(I++), Bits, What});
            }
            std::stable_sort(
                Regions.begin(), Regions.end(),
                [](const Region &A, const Region &B) { return A.Bit < B.Bit; });

            llvm::outs() << "  offset  bits  what\n";
            uint64_t Cursor = 0;
            auto row =
                [&](uint64_t Bit, uint64_t Bits, const std::string &What) {
                  llvm::outs()
                      << llvm::format(
                             "  %6s  %4llu  ",
                             (std::to_string((Bit) / 8) +
                              (((Bit) % 8) ? "." + std::to_string((Bit) % 8)
                                           : ""))
                                 .c_str(),
                             (unsigned long long)Bits)
                      << What << "\n";
                };
            for (const Region &R : Regions) {
              if (R.Bit > Cursor) {
                {
                  auto &&BitArgument4 = (Cursor);
                  auto &&BitsArgument4 = (R.Bit - Cursor);
                  auto &&WhatArgument4 = ("<padding>");

                  llvm::outs()
                      << llvm::format(
                             "  %6s  %4llu  ",
                             (std::to_string((BitArgument4) / 8) +
                              (((BitArgument4) % 8)
                                   ? "." + std::to_string((BitArgument4) % 8)
                                   : ""))
                                 .c_str(),
                             (unsigned long long)BitsArgument4)
                      << WhatArgument4 << "\n";
                }
              }
              {
                auto &&BitArgument5 = (R.Bit);
                auto &&BitsArgument5 = (R.Bits);
                auto &&WhatArgument5 = (R.What);

                llvm::outs()
                    << llvm::format(
                           "  %6s  %4llu  ",
                           (std::to_string((BitArgument5) / 8) +
                            (((BitArgument5) % 8)
                                 ? "." + std::to_string((BitArgument5) % 8)
                                 : ""))
                               .c_str(),
                           (unsigned long long)BitsArgument5)
                    << WhatArgument5 << "\n";
              }
              Cursor = std::max(
                  Cursor,
                  R.Bit + R.Bits); // unions overlap: keep the furthest end
            }
            uint64_t End = (uint64_t)L.getSize().getQuantity() * 8;
            if (Cursor < End) {
              {
                auto &&BitArgument6 = (Cursor);
                auto &&BitsArgument6 = (End - Cursor);
                auto &&WhatArgument6 = ("<tail padding>");

                llvm::outs()
                    << llvm::format(
                           "  %6s  %4llu  ",
                           (std::to_string((BitArgument6) / 8) +
                            (((BitArgument6) % 8)
                                 ? "." + std::to_string((BitArgument6) % 8)
                                 : ""))
                               .c_str(),
                           (unsigned long long)BitsArgument6)
                    << WhatArgument6 << "\n";
              }
            }
          }
        }
      }
    }
  }
  return 0;
}
