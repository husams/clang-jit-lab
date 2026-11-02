#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/Type.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <clang/Interpreter/Value.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
std::string safeString(const void *P) {
  std::string Out;
  for (int K = 0; K < 64; ++K) {
    char Ch;
#if defined(__APPLE__)
    vm_size_t Got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)P + K, 1,
                          (vm_address_t)&Ch, &Got) != KERN_SUCCESS ||
        Got != 1) {
      return "<unreadable>";
    }
#elif defined(__linux__)
    // Ask the kernel to copy from our address space so an invalid pointer or
    // a C string crossing an unmapped page reports EFAULT instead of crashing.
    iovec Local{&Ch, 1};
    iovec Remote{reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(P) + K),
                 1};
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
  ASTContext &C;
  llvm::raw_ostream &OS;
  const unsigned char *RootBegin = nullptr, *RootEnd = nullptr;

public:
  Inspector(ASTContext &C, llvm::raw_ostream &OS) : C(C), OS(OS) {}

  // Inspect the object of type T at Mem; pointers into it are reported as
  // offsets.
  void inspect(QualType T, const unsigned char *Mem, const std::string &Label) {
    RootBegin = Mem;
    RootEnd = Mem + C.getTypeSizeInChars(T).getQuantity();
    walk(T, Mem, 1, Label, 0);
  }

  // Print one object of type T living at Mem. Label is "name" for a field,
  // "[3]" for an element, "base Foo" for a base. Off is the offset from the
  // root object; Bit/Width describe a bit-field (Width == 0: not one).
  void walk(QualType T, const unsigned char *Mem, unsigned Depth,
            const std::string &Label, uint64_t Off, unsigned BitShift = 0,
            unsigned Width = 0) {
    // Canonicalize for dispatch: aliases should not change how a stored value
    // is read.
    QualType CT = T.getCanonicalType().getUnqualifiedType();
    std::string Head = std::string(Depth * 2, ' ') +
                       llvm::formatv("+{0,-3} ", Off).str() + Label + " : " +
                       typeName(T);

    if (const auto *BT = CT->getAs<BuiltinType>()) {
      OS << Head << " = " << scalar(BT, Mem, BitShift, Width) << "\n";
    } else if (const auto *ED = CT->getAsEnumDecl()) {
      OS << Head << " = " << enumerator(ED, Mem) << "\n";
    } else if (CT->isPointerType()) {
      const void *P;
      std::memcpy(&P, Mem, sizeof P);
      OS << Head << " = " << pointer(CT->getPointeeType(), P) << "\n";
    } else if (CT->isReferenceType()) {
      const void *P;
      std::memcpy(&P, Mem, sizeof P);
      OS << Head << " -> " << (llvm::formatv("{0:x}", (uintptr_t)P).str())
         << "\n";
    } else if (const auto *AT = C.getAsConstantArrayType(CT)) {
      array(AT, Head, Mem, Depth, Off);
    } else if (const CXXRecordDecl *RD = CT->getAsCXXRecordDecl()) {
      record(RD, Head, Mem, Depth, Off);
    } else {
      OS << Head << " = <not handled: " << CT->getTypeClassName() << ">\n";
    }
  }

private:
  // The type as written, except that an unnamed struct would print its source
  // location.
  std::string typeName(QualType T) {
    if (const RecordDecl *RD = T.getCanonicalType()->getAsRecordDecl()) {
      if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl()) {
        return RD->isUnion() ? "<anonymous union>" : "<anonymous struct>";
      }
    }
    return T.getAsString(C.getPrintingPolicy());
  }

  std::string scalar(const BuiltinType *BT, const unsigned char *Mem,
                     unsigned Shift, unsigned Width) {
    std::string S;
    llvm::raw_string_ostream R(S);
    auto get = [&](auto Zero) {
      decltype(Zero) V;
      std::memcpy(&V, Mem, sizeof V);
      return V;
    };
    if (Width) { // bit-field: pull the bits out of the first 8 bytes of the
                 // storage
      uint64_t Raw = 0;
      std::memcpy(&Raw, Mem, std::min<size_t>(8, (Shift + Width + 7) / 8));
      Raw = (Raw >> Shift) & (Width >= 64 ? ~0ULL : ((1ULL << Width) - 1));
      if (BT->isSignedInteger() && Width < 64 &&
          (Raw >> (Width - 1)) & 1) { // sign-extend
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

  std::string enumerator(const EnumDecl *ED, const unsigned char *Mem) {
    QualType UT = ED->getIntegerType().getCanonicalType();
    uint64_t Size = C.getTypeSizeInChars(UT).getQuantity();
    int64_t V = 0;
    std::memcpy(&V, Mem, Size); // little endian: low bytes first
    bool Signed = UT->isSignedIntegerType();
    if (Signed && Size < 8 && (V >> (Size * 8 - 1)) & 1) {
      V |= -(1LL << (Size * 8)); // sign-extend
    }
    for (const EnumConstantDecl *EC : ED->enumerators()) {
      if (EC->getInitVal().getExtValue() == V) {
        return EC->getNameAsString() + " (" + std::to_string(V) + ")";
      }
    }
    return "(" + std::to_string(V) + ") not an enumerator";
  }

  std::string pointer(QualType Pointee, const void *P) {
    if (!P) {
      return "null";
    }
    std::string S = (llvm::formatv("{0:x}", (uintptr_t)P).str());
    if (Pointee.getCanonicalType()->isCharType()) { // C string: follow it
      S += " " + safeString(P);
    }
    if (P >= RootBegin &&
        P < RootEnd) { // a pointer into the object being inspected
      S += "  (points into the root object at +" +
           std::to_string(static_cast<const unsigned char *>(P) - RootBegin) +
           ")";
    }
    return S;
  }

  void array(const ConstantArrayType *AT, const std::string &Head,
             const unsigned char *Mem, unsigned Depth, uint64_t Off) {
    QualType Elt = AT->getElementType();
    uint64_t N = AT->getSize().getZExtValue();
    uint64_t Sz = C.getTypeSizeInChars(Elt).getQuantity();
    if (Elt.getCanonicalType()->isCharType()) { // char[N]: show as a string
      OS << Head << " = \""
         << std::string(reinterpret_cast<const char *>(Mem),
                        strnlen(reinterpret_cast<const char *>(Mem), N))
         << "\"\n";
      return;
    }
    OS << Head << "\n";
    for (uint64_t K = 0; K < N; ++K) {
      walk(Elt, Mem + K * Sz, Depth + 1, "[" + std::to_string(K) + "]",
           Off + K * Sz);
    }
  }

  void record(const CXXRecordDecl *RD, const std::string &Head,
              const unsigned char *Mem, unsigned Depth, uint64_t Off) {
    RD = RD->getDefinition();
    if (!RD) {
      OS << Head << " = <incomplete type>\n";
      return;
    }
    // Use compiler-computed offsets for this target rather than hard-coded
    // struct layout.
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);
    OS << Head << "  (" << L.getSize().getQuantity() << " bytes, align "
       << L.getAlignment().getQuantity() << ")"
       << (RD->isUnion() ? "  union: every member overlays the same bytes" : "")
       << "\n";
    if (RD->isDynamicClass() && L.hasOwnVFPtr()) {
      const void *VP;
      std::memcpy(&VP, Mem, sizeof VP);
      OS << std::string((Depth + 1) * 2, ' ')
         << llvm::formatv("+{0,-3} ", Off).str()
         << "<vptr> = " << (llvm::formatv("{0:x}", (uintptr_t)VP).str())
         << "\n";
    }
    for (const CXXBaseSpecifier &B : RD->bases()) {
      const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
      uint64_t BOff = (B.isVirtual() ? L.getVBaseClassOffset(BRD)
                                     : L.getBaseClassOffset(BRD))
                          .getQuantity();
      walk(B.getType(), Mem + BOff, Depth + 1, "base", Off + BOff);
    }
    unsigned I = 0;
    for (const FieldDecl *F : RD->fields()) {
      // Offsets arrive in bits; bit-fields also need the within-byte offset and
      // declared width.
      uint64_t Bit = L.getFieldOffset(I++);
      if (F->isBitField()) {
        walk(F->getType(), Mem + Bit / 8, Depth + 1,
             ((F)->getIdentifier() ? (F)->getNameAsString() : "<unnamed>"),
             Off + Bit / 8, Bit % 8, F->getBitWidthValue());
      } else {
        walk(F->getType(), Mem + Bit / 8, Depth + 1,
             ((F)->getIdentifier() ? (F)->getNameAsString() : "<unnamed>"),
             Off + Bit / 8);
      }
    }
  }
};

} // namespace

// Keep inputs separate: each accepted input adds to the same compiler session.
const std::vector<std::string> SourceInputs = {
    R"cpp(
#include <string>
)cpp",
    R"cpp(
enum Color : unsigned char { Red, Green = 5, Blue };
)cpp",
    R"cpp(
enum class Level : short { Low = 1, High = 9 };
)cpp",
    R"cpp(
struct Vec2 {
  float x;
  float y;
};
)cpp",
    R"cpp(
struct Base {
  int id;
};
)cpp",
    R"cpp(
struct Shape : Base {
  Color color;
  Vec2 pos;
};
)cpp",
    R"cpp(
struct Scene {
  Shape main;
  Level lvl;
  char label[8];
  int grid[2][3];
  const char *title;
  Shape *next;
  unsigned flags : 4;
  unsigned mode : 2;
  int delta : 3;
  double scale;
};
)cpp",
    R"cpp(
Scene scene = {{{7}, Green, {1.5f, -2.0f}},
               Level::High,
               "hello",
               {{1, 2, 3}, {4, 5, 6}},
               "demo",
               nullptr,
               5,
               2,
               -1,
               0.25};
)cpp",
    R"cpp(
scene.main.pos.y = -2.5f;
)cpp",
    R"cpp(
scene.next = &scene.main;
)cpp",
    R"cpp(
std::string small = "short";
)cpp",
    R"cpp(
std::string big =
    "a string that is clearly longer than libc++'s inline buffer";
)cpp",
};

int main(int argc, const char **argv) {
  // Register typed host options before parsing argv; Clang receives its own
  // flag list.
  llvm::cl::OptionCategory Options("JIT example options");
  llvm::cl::list<std::string> ExpressionsOption(
      "expression", llvm::cl::ZeroOrMore,
      llvm::cl::desc("C++ expression to inspect; repeat for several"),
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

  std::vector<std::string> Expressions(ExpressionsOption.begin(),
                                       ExpressionsOption.end());
  if (Expressions.empty()) {
    Expressions = {"scene", "small", "big"};
  }
  CompilerFlags.insert(CompilerFlags.begin(), "-Wno-unused-value");
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

  for (size_t K = 0; K < Expressions.size(); ++K) {
    const std::string &Expr = Expressions[K];
    Value V;
    llvm::Error InputError2 = I->ParseAndExecute(Expr, &V);
    bool InputAccepted1 = !InputError2;
    if (InputError2) {
      llvm::logAllUnhandledErrors(std::move(InputError2), llvm::errs(),
                                  "error: ");
    }
    if (!InputAccepted1 || !V.isValid()) {
      continue;
    }
    ASTContext &C = I->getASTContext();
    // A class lvalue Value borrows its object; an array or temporary can own
    // separate storage.
    QualType T = V.getType().getNonReferenceType();
    llvm::outs() << Expr << "\n";
    if (V.getKind() != Value::K_PtrOrObj || T->isPointerType()) {
      std::string ValueText;
      if (!V.isValid()) {
        ValueText = "<invalid Value>\n";
      } else {
        llvm::raw_string_ostream OutputForValueText(ValueText);
        V.print(OutputForValueText);
      }
      const char *KindName = "?";
      switch (V.getKind()) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    KindName = "K_" #name;                                                     \
    break;
        REPL_BUILTIN_TYPES
#undef X
      case clang::Value::K_Void:
        KindName = "K_Void";
        break;
      case clang::Value::K_PtrOrObj:
        KindName = "K_PtrOrObj";
        break;
      case clang::Value::K_Unspecified:
        KindName = "K_Unspecified";
        break;
      }
      llvm::outs() << "  not an object in memory (kind " << KindName
                   << "): " << ValueText;
      continue;
    }
    // An lvalue record is a reference: getPtr() is the object itself.
    // An rvalue record or an array is a copy the Value owns: getPtr() is the
    // copy.
    Inspector(C, llvm::outs())
        .inspect(T, static_cast<const unsigned char *>(V.getPtr()), Expr);
  }
  return 0;
}
