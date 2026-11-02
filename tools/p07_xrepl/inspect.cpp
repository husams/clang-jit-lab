// inspect.cpp -- a field-by-field object viewer driven by ASTRecordLayout.
//
// The interpreter's memory is the process's own memory, so a JIT'd object is just bytes at an
// address; what the bytes mean comes from the AST: ASTContext::getASTRecordLayout() gives the
// offset of every field and base, exactly as CodeGen laid them out.
#include "inspect.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/Support/Format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace clang;

namespace {

struct Inspector {
  ASTContext &Ctx;
  llvm::raw_ostream &OS;
  const xrepl::InspectOptions &Opts;

  std::string typeName(QualType T) {
    // libc++ is full of anonymous structs; their default spelling contains a source path
    if (const RecordDecl *RD = T.getCanonicalType()->getAsRecordDecl())
      if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl())
        return std::string("<anonymous ") + RD->getKindName().str() + ">";
    std::string S = T.getAsString(Ctx.getPrintingPolicy());
    if (llvm::StringRef(S).starts_with("__libcpp")) // library-internal alias spelling: show the real type
      S = T.getCanonicalType().getAsString(Ctx.getPrintingPolicy());
    return S;
  }

  void line(int Depth, uint64_t Off, llvm::StringRef Name, QualType T) {
    OS.indent(2 + 2 * Depth) << llvm::format("+%-3llu ", (unsigned long long)Off) << Name << " : " << typeName(T);
  }

  static uint64_t readBits(const uint8_t *Mem, uint64_t BitOff, unsigned Width) {
    uint64_t V = 0; // little endian: bit 0 of byte 0 is the least significant bit
    for (unsigned I = 0; I < Width && I < 64; ++I) {
      uint64_t B = BitOff + I;
      V |= (uint64_t)((Mem[B / 8] >> (B % 8)) & 1) << I;
    }
    return V;
  }

  static std::string fmtDouble(double D) {
    char Buf[64];
    for (int P = 1; P <= 17; ++P) { // the shortest digits that read back as the same double
      std::snprintf(Buf, sizeof Buf, "%.*g", P, D);
      if (std::strtod(Buf, nullptr) == D)
        break;
    }
    return Buf;
  }

  static std::string charLit(uint64_t C) {
    switch (C) {
    case '\n': return "'\\n'";
    case '\t': return "'\\t'";
    case 0: return "'\\0'";
    case '\'': return "'\\''";
    case '\\': return "'\\\\'";
    }
    if (C >= 32 && C < 127)
      return std::string("'") + char(C) + "'";
    return "'\\x" + llvm::utohexstr(C) + "'";
  }

  // The text after "=" for a scalar of type T at P (a bit-field passes its own bit range).
  std::string scalar(QualType T, const uint8_t *P, uint64_t BitOff = 0, unsigned Width = 0) {
    QualType C = T.getCanonicalType().getUnqualifiedType();
    uint64_t Bytes = Ctx.getTypeSizeInChars(C).getQuantity();
    bool IsBitField = Width != 0;
    uint64_t Raw = 0;
    if (IsBitField)
      Raw = readBits(P, BitOff, Width);
    else
      std::memcpy(&Raw, P, std::min<uint64_t>(Bytes, 8));
    unsigned Bits = IsBitField ? Width : unsigned(std::min<uint64_t>(Bytes, 8) * 8);

    if (C->isNullPtrType())
      return "nullptr";
    if (C->isBooleanType())
      return Raw ? "true" : "false";
    if (const EnumDecl *ED = C->getAsEnumDecl()) {
      bool Signed = ED->getIntegerType()->isSignedIntegerType();
      int64_t V = Signed && Bits < 64 ? int64_t(Raw << (64 - Bits)) >> (64 - Bits) : int64_t(Raw);
      for (const EnumConstantDecl *EC : ED->enumerators())
        if (EC->getInitVal().getExtValue() == V)
          return EC->getName().str() + " (" + std::to_string(V) + ")";
      return "(" + typeName(T) + ")" + std::to_string(V);
    }
    if (C->isIntegralType(Ctx) || C->isCharType() || C->isWideCharType()) {
      bool Signed = C->isSignedIntegerType();
      std::string Num;
      if (Signed) {
        int64_t V = Bits < 64 ? int64_t(Raw << (64 - Bits)) >> (64 - Bits) : int64_t(Raw);
        Num = std::to_string(V);
      } else {
        Num = std::to_string(Raw);
      }
      if (C->isCharType())
        return charLit(Raw) + " (" + Num + ")";
      return Num;
    }
    if (C->isRealFloatingType()) {
      if (Bytes == 4) {
        float F;
        std::memcpy(&F, P, 4);
        return fmtDouble(F);
      }
      if (Bytes == 8) {
        double D;
        std::memcpy(&D, P, 8);
        return fmtDouble(D);
      }
      std::string S = "raw 0x";
      for (uint64_t I = Bytes; I-- > 0;)
        S += llvm::utohexstr(P[I], false, 2);
      return S;
    }
    if (C->isPointerType() || C->isReferenceType() || C->isBlockPointerType()) {
      void *Ptr;
      std::memcpy(&Ptr, P, sizeof Ptr);
      std::string S = Ptr ? "0x" + llvm::utohexstr(reinterpret_cast<uintptr_t>(Ptr)) : "nullptr";
      return C->isReferenceType() ? "-> " + S : S;
    }
    std::string S = "bytes";
    for (uint64_t I = 0; I < Bytes && I < 16; ++I)
      S += " " + llvm::utohexstr(P[I], false, 2);
    return S;
  }

  void record(const RecordDecl *RD, const uint8_t *P, uint64_t Off, int Depth) {
    RD = RD->getDefinition();
    if (!RD) {
      OS.indent(4 + 2 * Depth) << "<incomplete type>\n";
      return;
    }
    if (Depth >= int(Opts.MaxDepth)) {
      OS.indent(2 + 2 * Depth) << "...\n";
      return;
    }
    const ASTRecordLayout &L = Ctx.getASTRecordLayout(RD);
    const auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    bool Any = false;
    if (CXX && L.hasOwnVFPtr()) {
      Any = true;
      void *VP;
      std::memcpy(&VP, P, sizeof VP);
      OS.indent(2 + 2 * Depth) << llvm::format("+%-3llu ", (unsigned long long)Off) << "<vptr> = 0x"
                               << llvm::utohexstr(reinterpret_cast<uintptr_t>(VP)) << "\n";
    }
    if (CXX) {
      for (const CXXBaseSpecifier &B : CXX->bases()) {
        const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl();
        Any = true;
        if (B.isVirtual()) {
          OS.indent(2 + 2 * Depth) << "<virtual base " << BD->getName() << "> (not shown)\n";
          continue;
        }
        uint64_t BOff = L.getBaseClassOffset(BD).getQuantity();
        line(Depth, Off + BOff, "<base>", B.getType());
        OS << "  (" << Ctx.getTypeSizeInChars(B.getType()).getQuantity() << " bytes)\n";
        record(BD, P + BOff, Off + BOff, Depth + 1);
      }
    }
    unsigned Idx = 0;
    for (const FieldDecl *FD : RD->fields()) {
      Any = true;
      uint64_t BitOff = L.getFieldOffset(Idx++);
      std::string Name = FD->getName().empty() ? "<anonymous>" : FD->getName().str();
      if (FD->isBitField()) {
        unsigned W = FD->getBitWidthValue();
        line(Depth, BitOff / 8, Name, FD->getType());
        OS << " : " << W << " = " << scalar(FD->getType(), P, BitOff, W) << "\n";
        continue;
      }
      member(FD->getType(), Name, P + BitOff / 8, Off + BitOff / 8, Depth);
    }
    if (!Any)
      OS.indent(2 + 2 * Depth) << "(no data members)\n";
  }

  // What follows "name : type": "  (N bytes)" and the children of an aggregate, or " = value".
  void rest(QualType T, const uint8_t *P, uint64_t Off, int Depth) {
    QualType C = T.getCanonicalType();
    if (const RecordDecl *RD = C->getAsRecordDecl()) {
      OS << "  (" << Ctx.getTypeSizeInChars(C).getQuantity() << " bytes)\n";
      record(RD, P, Off, Depth + 1);
      return;
    }
    if (const ConstantArrayType *CAT = Ctx.getAsConstantArrayType(C)) {
      QualType E = CAT->getElementType();
      uint64_t N = CAT->getZExtSize();
      uint64_t ES = Ctx.getTypeSizeInChars(E).getQuantity();
      if (E.getCanonicalType()->isCharType()) {
        std::string S;
        for (uint64_t I = 0; I < N && P[I]; ++I)
          S += char(P[I]);
        OS << " = \"" << S << "\"" << (S.size() == N ? " (no terminator)" : "") << "\n";
        return;
      }
      OS << "  (" << N << " x " << ES << " bytes)\n";
      if (Depth + 1 >= int(Opts.MaxDepth)) {
        OS.indent(4 + 2 * Depth) << "...\n";
        return;
      }
      for (uint64_t I = 0; I < N && I < Opts.MaxElems; ++I)
        member(E, "[" + std::to_string(I) + "]", P + I * ES, Off + I * ES, Depth + 1);
      if (N > Opts.MaxElems)
        OS.indent(4 + 2 * Depth) << "... " << (N - Opts.MaxElems) << " more\n";
      return;
    }
    OS << " = " << scalar(T, P) << "\n";
  }

  void member(QualType T, llvm::StringRef Name, const uint8_t *P, uint64_t Off, int Depth) {
    line(Depth, Off, Name, T);
    rest(T, P, Off, Depth);
  }
};

} // namespace

namespace xrepl {

void inspectObject(ASTContext &Ctx, llvm::raw_ostream &OS, llvm::StringRef Label, QualType T, const uint8_t *Mem,
                   const InspectOptions &Opts) {
  Inspector In{Ctx, OS, Opts};
  OS << Label << " : " << In.typeName(T);
  In.rest(T, Mem, 0, -1); // children of the root sit at depth 0
}

} // namespace xrepl
