// p07_xrepl -- your own REPL: everything clang-repl does, plus %ast %ir %type %inspect %decls
// %complete %time and %undo [N] (Part 7).
//
// Usage: build/bin/p07_xrepl [-- compiler flags]
//        XREPL_DETERMINISTIC=1  makes %time print "<t>" instead of milliseconds
//        XREPL_HISTORY=path     history file for the line editor (default <lab>/out/p07_xrepl_history)
#include "commands.h"

#include "llvm/LineEditor/LineEditor.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

int main(int argc, const char **argv) {
  repllab::init();
  repllab::Args A = repllab::splitArgs(argc, argv);
  if (!A.Positional.empty()) {
    llvm::errs() << "usage: p07_xrepl [-- compiler flags]   (inputs are read from standard input)\n";
    return 2;
  }
  xrepl::Session S(A.Compiler);

  // LineEditor falls back to ~/.<name>-history when no path is given; keep the history in the lab.
  const char *H = std::getenv("XREPL_HISTORY");
  std::string History = H && *H ? H : REPLLAB_LAB_ROOT "/out/p07_xrepl_history";
  llvm::sys::fs::create_directories(llvm::sys::path::parent_path(History));

  llvm::LineEditor LE("xrepl", History);
  LE.setListCompleter(xrepl::TabCompleter{S});
  auto Prompt = [&](const char *P) { LE.setPrompt(P); };
  Prompt("xrepl> ");

  std::string Input;
  while (std::optional<std::string> Line = LE.readLine()) {
    llvm::StringRef L = llvm::StringRef(*Line).trim();
    // clang-repl's rules: a trailing backslash continues the input; in a preprocessor line the
    // newline matters, so it is kept.
    if (L.ends_with("\\")) {
      Input += L.drop_back().str();
      if (L.starts_with("#"))
        Input += "\n";
      Prompt("xrepl...   ");
      continue;
    }
    Input += L.str();
    Prompt("xrepl> ");
    std::string Done = std::move(Input);
    Input.clear();
    if (Done.empty()) // clang-repl turns an empty line into an empty input; xrepl ignores it
      continue;
    if (xrepl::dispatch(S, Done) == xrepl::Flow::Quit)
      break;
  }
  S.dropLastValue();
  return 0;
}
