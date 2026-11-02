// session.cpp -- Parse / Execute split, value hand-over, hidden evaluation.
#include "session.h"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Regex.h"

#include <chrono>

using namespace clang;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point T0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - T0).count();
}

} // namespace

namespace xrepl {

std::string maskIR(llvm::StringRef S) {
  // inttoptr (i64 41960603648 to ptr): the addresses of the Interpreter and the Value
  static const llvm::Regex Dec("i64 (noundef )?-?[0-9]{7,} to ptr");
  std::string Out = repllab::maskAddrs(S);
  std::string Err;
  while (Dec.match(Out))
    Out = Dec.sub("i64 ADDR to ptr", Out, &Err);
  return Out;
}

Session::Session(std::vector<std::string> CompilerArgs) : Args(std::move(CompilerArgs)) {
  const char *D = std::getenv("XREPL_DETERMINISTIC");
  Deterministic = D && *D == '1';
  llvm::ExitOnError ExitOnErr("xrepl: ");
  I = ExitOnErr(repllab::makeInterpreter(Args));
}

Session::~Session() { Last.clear(); }

bool Session::report(llvm::Error E) {
  llvm::logAllUnhandledErrors(std::move(E), llvm::errs(), "error: ");
  return false;
}

void Session::collectDecls(Record &R, TranslationUnitDecl *TU) {
  const SourceManager &SM = ctx().getSourceManager();
  for (Decl *D : TU->decls()) {
    if (D->isImplicit())
      continue;
    // Inputs are virtual files called input_line_N; synthesized declarations (the statement
    // wrapper of a value-printing expression) have no location at all.
    bool User = true;
    if (D->getLocation().isValid())
      User = SM.getFilename(SM.getExpansionLoc(D->getLocation())).starts_with("input_line_");
    R.Decls.push_back({D, User});
  }
}

// A semicolon-less expression leaves its result in Interpreter::LastValue, which is private.
// The public way to receive it is ParseAndExecute(code, &V): it hands the pending value to V
// whatever `code` is. An empty input does that without running anything, and Undo(1) removes the
// empty input again. Do it only when a value is pending, so inputs without one are not renumbered.
static bool valuePending(const PartialTranslationUnit &PTU) {
  for (const Decl *D : PTU.TUPart->decls())
    if (const auto *TL = dyn_cast<TopLevelStmtDecl>(D); TL && TL->isSemiMissing())
      return true;
  return false;
}

bool Session::fetchValue(Value &V) {
  if (llvm::Error E = I->ParseAndExecute("", &V)) {
    report(std::move(E));
    return false;
  }
  if (llvm::Error E = I->Undo(1))
    report(std::move(E));
  return V.isValid();
}

bool Session::submit(llvm::StringRef Code, Timing *T) {
  auto T0 = Clock::now();
  llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(Code);
  if (T)
    T->ParseMs = msSince(T0);
  if (!PTU)
    return report(PTU.takeError());

  Record &R = Records.emplace_back();
  R.Number = static_cast<unsigned>(Records.size());
  R.Source = Code.str();
  collectDecls(R, PTU->TUPart);
  if (PTU->TheModule) {
    llvm::raw_string_ostream OS(R.IR);
    PTU->TheModule->print(OS, nullptr); // before Execute: it moves the module into the JIT
    R.HasIR = true;
    auto T1 = Clock::now();
    llvm::Error E = I->Execute(*PTU);
    if (T)
      T->ExecMs = msSince(T1);
    if (E)
      return report(std::move(E));
  }

  Value V;
  if (valuePending(*PTU) && fetchValue(V)) {
    // Move-ASSIGN into a member that already exists: it is the correct operation.
    // (Value's move CONSTRUCTOR releases the object it was handed.)
    Last = std::move(V);
    if (!Last.isVoid())
      Last.dump(); // value printing, exactly what ParseAndExecute does without a Value*
    // clang-repl clears the Value right after printing it, which runs the object's destructor at
    // that moment. Do the same for objects; keep only values that own nothing (numbers, pointers).
    if (Last.isManuallyAlloc())
      Last.clear();
  }
  return true;
}

bool Session::undo(unsigned N) {
  if (llvm::Error E = I->Undo(N))
    return report(std::move(E));
  for (unsigned K = 0; K < N && !Records.empty(); ++K)
    Records.pop_back();
  Last.clear(); // it may name a type that was just undone
  return true;
}

bool Session::withHidden(const std::string &Code, llvm::function_ref<void(Value &)> Use, const std::string &Setup) {
  unsigned Hidden = 0; // inputs to undo at the end
  auto Cleanup = [&] {
    if (Hidden)
      if (llvm::Error U = I->Undo(Hidden))
        llvm::consumeError(std::move(U));
  };
  for (const std::string *Src : {&Setup, &Code}) {
    if (Src->empty())
      continue;
    llvm::Expected<PartialTranslationUnit &> PTU = I->Parse(*Src);
    if (!PTU) {
      Cleanup();
      return report(PTU.takeError());
    }
    ++Hidden;
    llvm::Error E = PTU->TheModule ? I->Execute(*PTU) : llvm::Error::success();
    if (E) {
      Cleanup();
      return report(std::move(E));
    }
    if (Src == &Code) {
      Value V;
      if (valuePending(*PTU))
        fetchValue(V);
      Use(V);
      V.clear(); // an object Value must be released while the code that destroys it is still loaded
    }
  }
  Cleanup();
  return true;
}

std::optional<TypeInfo> Session::typeOf(const std::string &Expr) {
  llvm::Expected<PartialTranslationUnit &> PTU = I->Parse("using __xrepl_ty = decltype((" + Expr + "));");
  if (!PTU) {
    report(PTU.takeError());
    return std::nullopt;
  }
  std::optional<TypeInfo> Out;
  for (Decl *D : PTU->TUPart->decls())
    if (auto *TA = dyn_cast<TypeAliasDecl>(D); TA && TA->getName() == "__xrepl_ty") {
      TypeInfo TI;
      TI.Written = TA->getUnderlyingType();
      if (const auto *DT = TI.Written->getAs<DecltypeType>())
        TI.Written = DT->getUnderlyingType(); // drop the decltype wrapper, keep the expression's own sugar
      TI.Object = TI.Written.getNonReferenceType();
      if (TI.Written->isLValueReferenceType())
        TI.Cat = TypeInfo::LValue;
      else if (TI.Written->isRValueReferenceType())
        TI.Cat = TypeInfo::XValue;
      if (!TI.Object->isFunctionType() && !TI.Object->isVoidType() && !TI.Object->isIncompleteType() &&
          !TI.Object->isDependentType()) {
        TI.Sized = true;
        TI.Size = ctx().getTypeSizeInChars(TI.Object).getQuantity();
        TI.Align = ctx().getTypeAlignInChars(TI.Object).getQuantity();
      }
      Out = TI;
    }
  if (llvm::Error E = I->Undo(1))
    report(std::move(E));
  return Out;
}

} // namespace xrepl
