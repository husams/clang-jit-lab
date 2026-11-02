// p05_ownership -- who owns the object behind a Value, and when does its
// destructor run (Section 5.3).
//
// Usage: build/bin/p05_ownership temp|lvalue|copies|bug|bug-destroy|safe
#include "../p05_anatomy/kinds.h"

using namespace clang;

// Host-side counter the interpreted destructor increments: lets the tool observe
// "the destructor ran" without touching an object that may already be gone.
extern "C" int p05_dtor_runs = 0;

// An object with a visible life cycle. The copy constructor prints too, so the
// copy that SetValueWithAlloc makes into the Value's storage is visible.
static const char *Prelude = R"(
#include <cstdio>
extern "C" int p05_dtor_runs;
struct T {
  int n;
  T(int n) : n(n) { printf("    T(%d)\n", n); }
  T(const T &o) : n(o.n) { printf("    T(const T&) copy of %d\n", o.n); }
  ~T() { printf("    ~T(%d)\n", n); n = -1; ++p05_dtor_runs; }
};
)";

static void say(const char *Msg) { llvm::outs() << Msg << "\n"; }

int main(int argc, const char **argv) {
  repllab::init();
  llvm::ExitOnError ExitOnErr("p05_ownership: ");
  repllab::Args A = repllab::splitArgs(argc, argv);
  std::string Mode = A.Positional.empty() ? "temp" : A.Positional[0];
  std::unique_ptr<Interpreter> I = ExitOnErr(repllab::makeInterpreter(A.Compiler));
  ExitOnErr(I->ParseAndExecute(Prelude));

  if (Mode == "temp") {
    say("evaluate T{7}:");
    Value V;
    repllab::run(*I, "T{7}", &V);
    llvm::outs() << "kind=" << p05::kindName(V.getKind()) << " manuallyAlloc=" << V.isManuallyAlloc()
                 << " type=" << V.getType().getAsString() << " n=" << *(int *)V.getPtr() << "\n";
    say("V goes out of scope:");
  } else if (Mode == "lvalue") {
    repllab::run(*I, "T t(1);");
    say("evaluate the lvalue t:");
    {
      Value V;
      repllab::run(*I, "t", &V);
      llvm::outs() << "kind=" << p05::kindName(V.getKind()) << " manuallyAlloc=" << V.isManuallyAlloc()
                   << " type=" << V.getType().getAsString(V.getASTContext().getPrintingPolicy()) << "\n";
      say("V goes out of scope:");
    }
    say("V is gone; t still lives in the interpreter:");
    repllab::run(*I, "t.n");
  } else if (Mode == "copies") {
    say("evaluate T{8}:");
    Value V;
    repllab::run(*I, "T{8}", &V);
    {
      Value C1 = V;
      Value C2 = C1;
      llvm::outs() << "three owners, same storage: " << (C1.getPtr() == V.getPtr() && C2.getPtr() == V.getPtr())
                   << ", dtor runs so far: " << p05_dtor_runs << "\n";
      say("C1 and C2 die:");
    }
    llvm::outs() << "dtor runs so far: " << p05_dtor_runs << "\n";
    say("V.clear():");
    V.clear();
    llvm::outs() << "dtor runs so far: " << p05_dtor_runs << ", V valid: " << V.isValid() << "\n";
    say("move-assignment is safe:");
    Value S1, S2;
    repllab::run(*I, "T{9}", &S1);
    S2 = std::move(S1);
    llvm::outs() << "dtor runs so far: " << p05_dtor_runs << ", S1 valid: " << S1.isValid()
                 << ", S2 n=" << *(int *)S2.getPtr() << "\n";
    say("S2 goes out of scope:");
  } else if (Mode == "bug") {
    Value A1;
    repllab::run(*I, "T{9}", &A1);
    int *Payload = (int *)A1.getPtr();
    llvm::outs() << "A1 owns n=" << *Payload << ", dtor runs: " << p05_dtor_runs << "\n";
    say("Value *B = new Value(std::move(A1)):");
    // The heap Value is never deleted on purpose: its destructor would release
    // storage that is already gone.
    Value *B = new Value(std::move(A1));
    llvm::outs() << "dtor runs: " << p05_dtor_runs << ", A1 valid: " << A1.isValid() << ", B valid: " << B->isValid()
                 << ", B manuallyAlloc: " << B->isManuallyAlloc() << "\n";
    // Undefined behaviour, shown on purpose: the block was freed (after the
    // destructor stored -1 into it), so what the allocator left there is not 9.
    llvm::outs() << "B still points at " << (B->getPtr() == Payload ? "the same payload" : "something else")
                 << ", and its n is " << (*Payload == 9 ? "still 9" : "no longer 9") << "\n";
  } else if (Mode == "bug-destroy") {
    // The same bug, but B is allowed to die: its destructor Release()s the
    // storage a second time.
    Value A1;
    repllab::run(*I, "T{9}", &A1);
    {
      Value B = std::move(A1);
      say("B goes out of scope:");
    }
    say("survived");
  } else if (Mode == "safe") {
    say("copy construction instead of move construction:");
    Value V;
    repllab::run(*I, "T{9}", &V);
    Value B = V; // retains: both own the object
    llvm::outs() << "dtor runs: " << p05_dtor_runs << ", B n=" << *(int *)B.getPtr() << "\n";
    say("both go out of scope:");
  }
  return 0;
}
