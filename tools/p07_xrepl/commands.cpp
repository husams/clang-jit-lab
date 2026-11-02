// commands.cpp -- command table, dispatcher and the handlers for %ast ... %time.
#include "commands.h"
#include "inspect.h"

#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/CodeCompletion.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <cstring>

using namespace clang;

namespace xrepl {
namespace {

struct Command {
  const char *Name;
  const char *Arg;  // shown in %help
  const char *Help;
  bool CodeArg;     // the argument is C++ text (Tab completes it)
  Flow (*Run)(Session &, llvm::StringRef Arg);
};

llvm::raw_ostream &out() { return llvm::outs(); }

bool fail(const llvm::Twine &Msg) {
  llvm::errs() << "error: " << Msg << "\n";
  return false;
}

// ---- %ast ------------------------------------------------------------------------------------

std::string dumpDecl(Session &S, const Decl *D) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  D->dump(OS); // ASTDumper over the declaration; no colours because the stream is not a terminal
  OS.flush();
  // The value-printing wrapper of an expression input bakes in pointers as decimal integer literals.
  static const llvm::Regex Dec("'(unsigned long( long)?)' [0-9]{7,}");
  std::string Err;
  while (Dec.match(Text))
    Text = Dec.sub("'\\1' ADDR", Text, &Err);
  return repllab::maskAddrs(Text);
}

Flow cmdAst(Session &S, llvm::StringRef Arg) {
  if (S.records().empty()) {
    out() << "(no input yet)\n";
    return Flow::Continue;
  }
  if (Arg.empty()) {
    const Record &R = S.records().back();
    unsigned Shown = 0, Headers = 0;
    for (const Record::Entry &E : R.Decls) {
      if (!E.User) {
        ++Headers;
        continue;
      }
      out() << dumpDecl(S, E.D);
      ++Shown;
    }
    if (!Shown && !Headers)
      out() << "(input " << R.Number << " declared nothing)\n";
    if (Headers)
      out() << "(" << Headers << " declarations from included files not shown; name one with %ast <name>)\n";
    return Flow::Continue;
  }
  // %ast name: the newest declaration with that name, user code before headers
  for (bool UserOnly : {true, false})
    for (auto It = S.records().rbegin(); It != S.records().rend(); ++It)
      for (const Record::Entry &E : It->Decls)
        if (E.User == UserOnly)
          if (const auto *ND = dyn_cast<NamedDecl>(E.D); ND && ND->getNameAsString() == Arg) {
            out() << dumpDecl(S, E.D);
            return Flow::Continue;
          }
  fail("no declaration named '" + Arg + "'");
  return Flow::Continue;
}

// ---- %ir -------------------------------------------------------------------------------------

// The module header, the metadata and the attribute groups are the same in every module; hide them.
std::string trimIR(llvm::StringRef IR) {
  std::string Out;
  bool PrevBlank = true;
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  IR.split(Lines, '\n');
  for (llvm::StringRef L : Lines) {
    if (L.starts_with("; ModuleID") || L.starts_with("source_filename") || L.starts_with("target ") ||
        L.starts_with("attributes #") || L.starts_with("!"))
      continue;
    bool Blank = L.trim().empty();
    if (Blank && PrevBlank)
      continue;
    PrevBlank = Blank;
    Out += L.str() + "\n";
  }
  while (!Out.empty() && Out.back() == '\n')
    Out.pop_back();
  return Out.empty() ? Out : Out + "\n";
}

Flow cmdIr(Session &S, llvm::StringRef Arg) {
  if (S.records().empty()) {
    out() << "(no input yet)\n";
    return Flow::Continue;
  }
  if (!Arg.empty() && Arg != "full") {
    fail("usage: %ir [full]");
    return Flow::Continue;
  }
  const Record &R = S.records().back();
  if (!R.HasIR) {
    out() << "(input " << R.Number << " produced no module)\n";
    return Flow::Continue;
  }
  std::string Text = Arg == "full" ? R.IR : trimIR(R.IR);
  if (Text.empty())
    Text = "(input " + std::to_string(R.Number) + " generated no code)\n";
  out() << maskIR(Text);
  return Flow::Continue;
}

// ---- %type / %inspect ------------------------------------------------------------------------

const char *categoryName(TypeInfo::Category C) {
  switch (C) {
  case TypeInfo::LValue: return "lvalue";
  case TypeInfo::XValue: return "xvalue";
  case TypeInfo::PRValue: return "prvalue";
  }
  return "?";
}

Flow cmdType(Session &S, llvm::StringRef Arg) {
  if (Arg.empty()) {
    fail("usage: %type <expression>");
    return Flow::Continue;
  }
  std::optional<TypeInfo> TI = S.typeOf(Arg.str());
  if (!TI)
    return Flow::Continue;
  const PrintingPolicy &PP = S.ctx().getPrintingPolicy();
  out() << "type     : " << TI->Object.getAsString(PP) << "\n";
  out() << "canonical: " << TI->Object.getCanonicalType().getAsString(PP) << "\n";
  out() << "category : " << categoryName(TI->Cat) << "\n";
  if (TI->Sized)
    out() << "size     : " << TI->Size << " bytes, align " << TI->Align << "\n";
  else
    out() << "size     : n/a (" << (TI->Object->isFunctionType() ? "function" : "no object") << ")\n";
  return Flow::Continue;
}

// Copy the bytes of a builtin or pointer Value into a buffer, so a prvalue can be inspected like memory.
bool scalarBytes(const Value &V, uint8_t (&Buf)[16]) {
  std::memset(Buf, 0, sizeof Buf);
  switch (V.getKind()) {
#define X(type, name)                                                                                                  \
  case Value::K_##name: {                                                                                              \
    type Val = V.get##name();                                                                                          \
    std::memcpy(Buf, &Val, sizeof Val);                                                                                \
    return true;                                                                                                       \
  }
    REPL_BUILTIN_TYPES
#undef X
  case Value::K_PtrOrObj: {
    void *P = V.getPtr();
    std::memcpy(Buf, &P, sizeof P);
    return true;
  }
  default:
    return false;
  }
}

Flow cmdInspect(Session &S, llvm::StringRef Arg) {
  std::string Expr = Arg.str();
  if (Expr.empty()) {
    // no argument: the value of the last expression input, which the session kept
    Value &V = S.lastValue();
    if (!V.hasValue()) {
      fail("no value kept (objects are destroyed right after printing); use %inspect <expression>");
      return Flow::Continue;
    }
    uint8_t Buf[16];
    QualType T = V.getType();
    bool Ref = T->isReferenceType(); // the Value of `r` is a pointer to r
    const uint8_t *Mem = V.isManuallyAlloc() || Ref ? static_cast<const uint8_t *>(V.getPtr())
                                                    : (scalarBytes(V, Buf), Buf);
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    inspectObject(S.ctx(), OS, "$last", T.getNonReferenceType(), Mem);
    out() << repllab::maskAddrs(OS.str());
    return Flow::Continue;
  }
  std::optional<TypeInfo> TI = S.typeOf(Expr);
  if (!TI)
    return Flow::Continue;
  if (!TI->Sized) {
    fail("'" + Expr + "' has no object to inspect (function, void or incomplete type)");
    return Flow::Continue;
  }
  auto Show = [&](const uint8_t *Mem) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    inspectObject(S.ctx(), OS, Expr, TI->Object, Mem);
    out() << repllab::maskAddrs(OS.str());
  };
  if (TI->Cat != TypeInfo::PRValue) {
    // An lvalue or xvalue names an object that already exists: ask the JIT for its address and read it
    // in place. __builtin_addressof ignores an overloaded operator& (the leading &* only avoids an unused-value warning). An xvalue (std::move(v)) is not
    // addressable, so a reference is bound to it first; binding moves nothing.
    bool X = TI->Cat == TypeInfo::XValue;
    S.withHidden(X ? "&*__builtin_addressof(__xrepl_ref)" : "&*__builtin_addressof(" + Expr + ")",
                 [&](Value &V) {
                   if (V.getKind() == Value::K_PtrOrObj && V.getPtr())
                     Show(static_cast<const uint8_t *>(V.getPtr()));
                 },
                 X ? "auto &&__xrepl_ref = " + Expr + ";" : std::string());
  } else {
    // A temporary has no address; evaluate it and look at the copy the interpreter made.
    S.withHidden(Expr, [&](Value &V) {
      uint8_t Buf[16];
      if (V.isManuallyAlloc())
        Show(static_cast<const uint8_t *>(V.getPtr()));
      else if (scalarBytes(V, Buf))
        Show(Buf);
    });
  }
  return Flow::Continue;
}

// ---- %decls ----------------------------------------------------------------------------------

const char *kindName(const Decl *D) {
  switch (D->getKind()) {
  case Decl::Var: return "variable";
  case Decl::Function: return "function";
  case Decl::FunctionTemplate: return "function template";
  case Decl::ClassTemplate: return "class template";
  case Decl::VarTemplate: return "variable template";
  case Decl::TypeAlias: return "alias";
  case Decl::Typedef: return "typedef";
  case Decl::Enum: return "enum";
  case Decl::Namespace: return "namespace";
  case Decl::CXXRecord:
  case Decl::Record:
    switch (cast<TagDecl>(D)->getTagKind()) {
    case TagTypeKind::Struct: return "struct";
    case TagTypeKind::Class: return "class";
    case TagTypeKind::Union: return "union";
    default: return "record";
    }
  case Decl::Using: return "using";
  default: return D->getDeclKindName();
  }
}

void listDecl(const Record &R, const Decl *D, bool &First) {
  if (const auto *LS = dyn_cast<LinkageSpecDecl>(D)) { // extern "C" { ... }
    for (const Decl *Inner : LS->decls())
      listDecl(R, Inner, First);
    return;
  }
  const auto *ND = dyn_cast<NamedDecl>(D);
  if (!ND || ND->getDeclName().isEmpty()) // statements and anonymous declarations
    return;
  out() << llvm::left_justify(First ? std::to_string(R.Number) : std::string(), 6)
        << llvm::left_justify(kindName(D), 19) << ND->getNameAsString() << "\n";
  First = false;
}

Flow cmdDecls(Session &S, llvm::StringRef) {
  out() << llvm::left_justify("input", 6) << llvm::left_justify("kind", 19) << "name\n";
  for (const Record &R : S.records()) {
    bool First = true;
    unsigned Headers = 0;
    for (const Record::Entry &E : R.Decls) {
      if (!E.User) {
        ++Headers;
        continue;
      }
      listDecl(R, E.D, First);
    }
    if (Headers)
      out() << llvm::left_justify(First ? std::to_string(R.Number) : std::string(), 6) << "(+" << Headers
            << " declarations from included files)\n";
  }
  return Flow::Continue;
}

// ---- %complete and Tab -----------------------------------------------------------------------

Flow cmdComplete(Session &S, llvm::StringRef Arg);

// ---- %time -----------------------------------------------------------------------------------

Flow cmdTime(Session &S, llvm::StringRef Arg) {
  if (Arg.empty() || Arg.starts_with("%")) {
    fail("usage: %time <C++ input>");
    return Flow::Continue;
  }
  Timing T;
  if (!S.submit(Arg, &T))
    return Flow::Continue;
  if (S.deterministic())
    out() << "parse: <t> exec: <t>\n";
  else
    out() << "parse: " << llvm::format("%.3f", T.ParseMs) << " ms exec: " << llvm::format("%.3f", T.ExecMs)
          << " ms\n";
  return Flow::Continue;
}

// ---- %undo, %lib, %help, %quit ---------------------------------------------------------------

Flow cmdUndo(Session &S, llvm::StringRef Arg) {
  unsigned N = 1;
  if (!Arg.empty() && (Arg.getAsInteger(10, N) || N == 0)) {
    fail("usage: %undo [N]");
    return Flow::Continue;
  }
  S.undo(N);
  return Flow::Continue;
}

Flow cmdLib(Session &S, llvm::StringRef Arg) {
  if (Arg.empty()) {
    fail("%lib expects 1 argument: the path to a dynamic library");
    return Flow::Continue;
  }
  std::string Path = Arg.str();
  if (llvm::Error E = S.interp().LoadDynamicLibrary(Path.c_str()))
    S.report(std::move(E));
  return Flow::Continue;
}

Flow cmdQuit(Session &, llvm::StringRef) { return Flow::Quit; }
Flow cmdHelp(Session &, llvm::StringRef);

const Command Commands[] = {
    {"help", "", "list xrepl %commands", false, cmdHelp},
    {"undo", " [N]", "undo the previous N inputs (default 1)", false, cmdUndo},
    {"lib", " <path>", "link a dynamic library", false, cmdLib},
    {"quit", "", "exit xrepl", false, cmdQuit},
    {"ast", " [name]", "AST of the last input, or of the named declaration", false, cmdAst},
    {"ir", " [full]", "LLVM IR of the last input (captured before it ran)", false, cmdIr},
    {"type", " <expr>", "static type, size and value category; the expression is not run", true, cmdType},
    {"inspect", " [expr]", "fields of an object in JIT memory (no argument: the last value)", true, cmdInspect},
    {"decls", "", "every declaration so far, grouped by input", false, cmdDecls},
    {"complete", " <text>", "completion candidates for text, as Tab shows them", true, cmdComplete},
    {"time", " <input>", "run an input and report parse and execute time", true, cmdTime},
};

Flow cmdHelp(Session &, llvm::StringRef) {
  for (const Command &C : Commands)
    out() << llvm::left_justify(std::string("%") + C.Name + C.Arg, 18) << C.Help << "\n";
  return Flow::Continue;
}

Flow cmdComplete(Session &S, llvm::StringRef Arg) {
  std::string Prefix;
  llvm::Error Err = llvm::Error::success();
  std::vector<std::string> R = completeText(S, Arg, Prefix, Err);
  if (Err) {
    S.report(std::move(Err));
    return Flow::Continue;
  }
  if (R.empty()) {
    out() << "(no candidates)\n";
    return Flow::Continue;
  }
  constexpr size_t Limit = 20;
  for (size_t I = 0; I < R.size() && I < Limit; ++I)
    out() << R[I] << "\n";
  if (R.size() > Limit)
    out() << "... " << (R.size() - Limit) << " more\n";
  return Flow::Continue;
}

} // namespace

// ---- completion ------------------------------------------------------------------------------

std::vector<std::string> completeText(Session &S, llvm::StringRef Text, std::string &Prefix, llvm::Error &Err) {
  // ReplCodeCompleter needs a fresh CompilerInstance for the completion run (the code is parsed
  // up to the cursor in it), plus the main interpreter's CompilerInstance to see the session's names.
  llvm::ErrorAsOutParameter EAO(&Err);
  auto CI = repllab::makeCompiler(S.compilerArgs());
  if (!CI) {
    Err = CI.takeError();
    return {};
  }
  auto Fresh = Interpreter::create(std::move(*CI));
  if (!Fresh) {
    Err = Fresh.takeError();
    return {};
  }
  size_t Pos = Text.size();
  size_t Lines = std::count(Text.begin(), Text.end(), '\n') + 1;
  std::vector<std::string> Results;
  ReplCodeCompleter CC;
  CC.codeComplete((*Fresh)->getCompilerInstance(), Text, Lines, Pos + 1, S.interp().getCompilerInstance(), Results);
  Prefix = CC.Prefix;
  std::vector<std::string> Out;
  for (const std::string &C : Results)
    if (C.find(CC.Prefix) == 0) // keep what extends the word being typed
      Out.push_back(C);
  std::sort(Out.begin(), Out.end());
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
  return Out;
}

std::vector<llvm::LineEditor::Completion> TabCompleter::operator()(llvm::StringRef Buffer, size_t Pos) const {
  Buffer = Buffer.take_front(Pos);
  std::vector<llvm::LineEditor::Completion> Comps;
  if (Buffer.starts_with("%")) {
    size_t Sp = Buffer.find(' ');
    if (Sp == llvm::StringRef::npos) { // still typing the command name
      llvm::StringRef Typed = Buffer.drop_front();
      for (const Command &C : Commands)
        if (llvm::StringRef(C.Name).starts_with(Typed))
          Comps.emplace_back(std::string(C.Name).substr(Typed.size()), std::string("%") + C.Name);
      return Comps;
    }
    llvm::StringRef Name = Buffer.slice(1, Sp);
    for (const Command &C : Commands)
      if (C.Name == Name && C.CodeArg)
        Buffer = Buffer.drop_front(Sp + 1); // complete the C++ after the command
    if (Buffer.starts_with("%"))
      return Comps;
  }
  std::string Prefix;
  llvm::Error Err = llvm::Error::success();
  for (const std::string &C : completeText(S, Buffer, Prefix, Err))
    Comps.emplace_back(C.substr(Prefix.size()), C);
  if (Err)
    S.report(std::move(Err));
  return Comps;
}

// ---- dispatch --------------------------------------------------------------------------------

Flow dispatch(Session &S, llvm::StringRef Input) {
  if (!Input.starts_with("%")) {
    S.submit(Input);
    return Flow::Continue;
  }
  llvm::StringRef Rest = Input.drop_front();
  auto [Name, Arg] = Rest.split(' ');
  for (const Command &C : Commands)
    if (Name == C.Name)
      return C.Run(S, Arg.trim());
  // clang-repl's wording, to the letter: scripts/session.py uses this message as its end-of-input marker.
  S.report(llvm::make_error<llvm::StringError>(
      llvm::formatv("Invalid % command \"{0}\", use \"%help\" to list commands\n", Input), std::error_code()));
  return Flow::Continue;
}

} // namespace xrepl
