// inspect.h -- %inspect: read an object out of memory using the AST record layout.
#ifndef XREPL_INSPECT_H
#define XREPL_INSPECT_H

#include "clang/AST/ASTContext.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>

namespace xrepl {

struct InspectOptions {
  unsigned MaxDepth = 4;  // nesting levels of records and arrays that are expanded
  unsigned MaxElems = 8;  // array elements shown before "... N more"
};

// Print `Label : Type (size, align)` and the contents of the object of type `T` that lives at `Mem`.
void inspectObject(clang::ASTContext &Ctx, llvm::raw_ostream &OS, llvm::StringRef Label, clang::QualType T,
                   const uint8_t *Mem, const InspectOptions &Opts = {});

} // namespace xrepl

#endif
