// kinds.h -- shared by the Part 5 tools: Value::Kind -> name, built from the
// same X-macro that Value.h uses (Section 5.2).
#ifndef P05_KINDS_H
#define P05_KINDS_H

#include "repllab.h"

namespace p05 {

inline const char *kindName(clang::Value::Kind K) {
  switch (K) {
#define X(type, name)                                                          \
  case clang::Value::K_##name:                                                 \
    return "K_" #name;
    REPL_BUILTIN_TYPES
#undef X
  case clang::Value::K_Void:
    return "K_Void";
  case clang::Value::K_PtrOrObj:
    return "K_PtrOrObj";
  case clang::Value::K_Unspecified:
    return "K_Unspecified";
  }
  return "?";
}

} // namespace p05

#endif
