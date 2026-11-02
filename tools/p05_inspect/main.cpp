// p05_inspect -- an object inspector. Evaluate an expression to a Value, take
// the object's address from it, and walk the type's record layout, reading
// every field straight out of the interpreter's memory (Section 5.9).
//
// Usage: build/bin/p05_inspect setup.repl expr... [-- compiler flags]
#include <cstdint>
#include <cstring>

#include "../p05_anatomy/kinds.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

using namespace clang;

namespace {

// A C string at P, or a note if P is not readable memory. The inspector follows
// every char * it meets, and a union can make any bytes look like a pointer.
std::string safeString(const void* P) {
  std::string Out;
  for (int K = 0; K < 64; ++K) {
    char Ch;
#if defined(__APPLE__)
    vm_size_t Got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)P + K, 1, (vm_address_t)&Ch, &Got) !=
            KERN_SUCCESS ||
        Got != 1) {
      return "<unreadable>";
    }
#elif defined(__linux__)
    // Ask the kernel to copy from our address space so an invalid pointer or
    // a C string crossing an unmapped page reports EFAULT instead of crashing.
    iovec Local{&Ch, 1};
    iovec Remote{reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(P) + K), 1};
    if (process_vm_readv(getpid(), &Local, 1, &Remote, 1, 0) != 1) {
      return "<unreadable>";
    }
#else
    return "<unsupported platform>";
#endif
    if (!Ch) {
      return "\"" + Out + "\"";
    }
    Out += Ch;
  }
  return "\"" + Out + "\"...";
}

class Inspector {
  ASTContext& C;
  llvm::raw_ostream& OS;
  const unsigned char *RootBegin = nullptr, *RootEnd = nullptr;

public:
  Inspector(ASTContext& C, llvm::raw_ostream& OS)
      : C(C)
      , OS(OS) {}

  // Inspect the object of type T at Mem; pointers into it are reported as offsets.
  void inspect(QualType T, const unsigned char* Mem, const std::string& Label) {
    RootBegin = Mem;
    RootEnd = Mem + C.getTypeSizeInChars(T).getQuantity();
    walk(T, Mem, 1, Label, 0);
  }

  // Print one object of type T living at Mem. Label is "name" for a field,
  // "[3]" for an element, "base Foo" for a base. Off is the offset from the
  // root object; Bit/Width describe a bit-field (Width == 0: not one).
  void walk(QualType T, const unsigned char* Mem, unsigned Depth, const std::string& Label,
            uint64_t Off, unsigned BitShift = 0, unsigned Width = 0) {
    QualType CT = T.getCanonicalType().getUnqualifiedType();
    std::string Head = std::string(Depth * 2, ' ') + llvm::formatv("+{0,-3} ", Off).str() + Label +
                       " : " + typeName(T);

    if (const auto* BT = CT->getAs<BuiltinType>()) {
      OS << Head << " = " << scalar(BT, Mem, BitShift, Width) << "\n";
    } else if (const auto* ED = CT->getAsEnumDecl()) {
      OS << Head << " = " << enumerator(ED, Mem) << "\n";
    } else if (CT->isPointerType()) {
      const void* P;
      std::memcpy(&P, Mem, sizeof P);
      OS << Head << " = " << pointer(CT->getPointeeType(), P) << "\n";
    } else if (CT->isReferenceType()) {
      const void* P;
      std::memcpy(&P, Mem, sizeof P);
      OS << Head << " -> " << repllab::maskAddrs(llvm::formatv("{0:x}", (uintptr_t)P).str())
         << "\n";
    } else if (const auto* AT = C.getAsConstantArrayType(CT)) {
      array(AT, Head, Mem, Depth, Off);
    } else if (const CXXRecordDecl* RD = CT->getAsCXXRecordDecl()) {
      record(RD, Head, Mem, Depth, Off);
    } else {
      OS << Head << " = <not handled: " << CT->getTypeClassName() << ">\n";
    }
  }

private:
  // The type as written, except that an unnamed struct would print its source location.
  std::string typeName(QualType T) {
    if (const RecordDecl* RD = T.getCanonicalType()->getAsRecordDecl()) {
      if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl()) {
        return RD->isUnion() ? "<anonymous union>" : "<anonymous struct>";
      }
    }
    return T.getAsString(C.getPrintingPolicy());
  }

  static std::string name(const FieldDecl* F) {
    return F->getIdentifier() ? F->getNameAsString() : "<unnamed>";
  }

  std::string scalar(const BuiltinType* BT, const unsigned char* Mem, unsigned Shift,
                     unsigned Width) {
    std::string S;
    llvm::raw_string_ostream R(S);
    auto get = [&](auto Zero) {
      decltype(Zero) V;
      std::memcpy(&V, Mem, sizeof V);
      return V;
    };
    if (Width) { // bit-field: pull the bits out of the first 8 bytes of the storage
      uint64_t Raw = 0;
      std::memcpy(&Raw, Mem, std::min<size_t>(8, (Shift + Width + 7) / 8));
      Raw = (Raw >> Shift) & (Width >= 64 ? ~0ULL : ((1ULL << Width) - 1));
      if (BT->isSignedInteger() && Width < 64 && (Raw >> (Width - 1)) & 1) { // sign-extend
        R << (int64_t)(Raw | ~((1ULL << Width) - 1));
      } else {
        R << Raw;
      }
      R << "  (" << Width << " bits at bit " << Shift << ")";
      return S;
    }
    switch (BT->getKind()) {
    case BuiltinType::Bool:
      R << (get(false) ? "true" : "false");
      break;
    case BuiltinType::Char_S:
    case BuiltinType::SChar: {
      signed char V = get((signed char)0);
      R << (int)V;
      if (V >= 32 && V < 127) {
        R << " '" << (char)V << "'";
      }
      break;
    }
    case BuiltinType::Char_U:
    case BuiltinType::UChar: {
      unsigned char V = get((unsigned char)0);
      R << (unsigned)V;
      if (V >= 32 && V < 127) {
        R << " '" << (char)V << "'";
      }
      break;
    }
    case BuiltinType::Short:
      R << get((short)0);
      break;
    case BuiltinType::UShort:
      R << get((unsigned short)0);
      break;
    case BuiltinType::Int:
      R << get(0);
      break;
    case BuiltinType::UInt:
      R << get(0u);
      break;
    case BuiltinType::Long:
      R << get(0L);
      break;
    case BuiltinType::ULong:
      R << get(0UL);
      break;
    case BuiltinType::LongLong:
      R << get(0LL);
      break;
    case BuiltinType::ULongLong:
      R << get(0ULL);
      break;
    case BuiltinType::Float:
      R << llvm::format("%g", (double)get(0.0f));
      break;
    case BuiltinType::Double:
      R << llvm::format("%g", get(0.0));
      break;
    case BuiltinType::NullPtr:
      R << "nullptr";
      break;
    default:
      R << "<builtin kind " << (int)BT->getKind() << ">";
    }
    return S;
  }

  std::string enumerator(const EnumDecl* ED, const unsigned char* Mem) {
    QualType UT = ED->getIntegerType().getCanonicalType();
    uint64_t Size = C.getTypeSizeInChars(UT).getQuantity();
    int64_t V = 0;
    std::memcpy(&V, Mem, Size); // little endian: low bytes first
    bool Signed = UT->isSignedIntegerType();
    if (Signed && Size < 8 && (V >> (Size * 8 - 1)) & 1) {
      V |= -(1LL << (Size * 8)); // sign-extend
    }
    for (const EnumConstantDecl* EC : ED->enumerators()) {
      if (EC->getInitVal().getExtValue() == V) {
        return EC->getNameAsString() + " (" + std::to_string(V) + ")";
      }
    }
    return "(" + std::to_string(V) + ") not an enumerator";
  }

  std::string pointer(QualType Pointee, const void* P) {
    if (!P) {
      return "null";
    }
    std::string S = repllab::maskAddrs(llvm::formatv("{0:x}", (uintptr_t)P).str());
    if (Pointee.getCanonicalType()->isCharType()) { // C string: follow it
      S += " " + safeString(P);
    }
    if (P >= RootBegin && P < RootEnd) { // a pointer into the object being inspected
      S += "  (points into the root object at +" +
           std::to_string(static_cast<const unsigned char*>(P) - RootBegin) + ")";
    }
    return S;
  }

  void array(const ConstantArrayType* AT, const std::string& Head, const unsigned char* Mem,
             unsigned Depth, uint64_t Off) {
    QualType Elt = AT->getElementType();
    uint64_t N = AT->getSize().getZExtValue();
    uint64_t Sz = C.getTypeSizeInChars(Elt).getQuantity();
    if (Elt.getCanonicalType()->isCharType()) { // char[N]: show as a string
      OS << Head << " = \""
         << std::string(reinterpret_cast<const char*>(Mem),
                        strnlen(reinterpret_cast<const char*>(Mem), N))
         << "\"\n";
      return;
    }
    OS << Head << "\n";
    for (uint64_t K = 0; K < N; ++K) {
      walk(Elt, Mem + K * Sz, Depth + 1, "[" + std::to_string(K) + "]", Off + K * Sz);
    }
  }

  void record(const CXXRecordDecl* RD, const std::string& Head, const unsigned char* Mem,
              unsigned Depth, uint64_t Off) {
    RD = RD->getDefinition();
    if (!RD) {
      OS << Head << " = <incomplete type>\n";
      return;
    }
    const ASTRecordLayout& L = C.getASTRecordLayout(RD);
    OS << Head << "  (" << L.getSize().getQuantity() << " bytes, align "
       << L.getAlignment().getQuantity() << ")"
       << (RD->isUnion() ? "  union: every member overlays the same bytes" : "") << "\n";
    if (RD->isDynamicClass() && L.hasOwnVFPtr()) {
      const void* VP;
      std::memcpy(&VP, Mem, sizeof VP);
      OS << std::string((Depth + 1) * 2, ' ') << llvm::formatv("+{0,-3} ", Off).str()
         << "<vptr> = " << repllab::maskAddrs(llvm::formatv("{0:x}", (uintptr_t)VP).str()) << "\n";
    }
    for (const CXXBaseSpecifier& B : RD->bases()) {
      const CXXRecordDecl* BRD = B.getType()->getAsCXXRecordDecl();
      uint64_t BOff =
          (B.isVirtual() ? L.getVBaseClassOffset(BRD) : L.getBaseClassOffset(BRD)).getQuantity();
      walk(B.getType(), Mem + BOff, Depth + 1, "base", Off + BOff);
    }
    unsigned I = 0;
    for (const FieldDecl* F : RD->fields()) {
      uint64_t Bit = L.getFieldOffset(I++);
      if (F->isBitField()) {
        walk(F->getType(), Mem + Bit / 8, Depth + 1, name(F), Off + Bit / 8, Bit % 8,
             F->getBitWidthValue());
      } else {
        walk(F->getType(), Mem + Bit / 8, Depth + 1, name(F), Off + Bit / 8);
      }
    }
  }
};

} // namespace

int main(int argc, const char** argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_inspect: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (A.Positional.size() < 2) {
    llvm::errs() << "usage: p05_inspect setup.repl expr...\n";
    return 2;
  }
  A.Compiler.insert(A.Compiler.begin(), "-Wno-unused-value");
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  for (const std::string& Code : repllab::replInputs(repllab::readFile(A.Positional[0]))) {
    ExitOnErr(I->ParseAndExecute(Code));
  }

  for (size_t K = 1; K < A.Positional.size(); ++K) {
    const std::string& Expr = A.Positional[K];
    Value V;
    if (!repllab::run(*I, Expr, &V) || !V.isValid()) {
      continue;
    }
    ASTContext& C = I->getASTContext();
    QualType T = V.getType().getNonReferenceType();
    llvm::outs() << Expr << "\n";
    if (V.getKind() != Value::K_PtrOrObj || T->isPointerType()) {
      llvm::outs() << "  not an object in memory (kind " << p05::kindName(V.getKind())
                   << "): " << repllab::valueString(V);
      continue;
    }
    // An lvalue record is a reference: getPtr() is the object itself.
    // An rvalue record or an array is a copy the Value owns: getPtr() is the copy.
    Inspector(C, llvm::outs()).inspect(T, static_cast<const unsigned char*>(V.getPtr()), Expr);
  }
  return 0;
}
