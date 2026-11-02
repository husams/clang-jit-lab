// commands.h -- the % commands and the Tab completer.
#ifndef XREPL_COMMANDS_H
#define XREPL_COMMANDS_H

#include "session.h"

#include "llvm/LineEditor/LineEditor.h"

namespace xrepl {

enum class Flow { Continue, Quit };

// One complete input (already assembled from continuation lines): a % command or C++ code.
Flow dispatch(Session &S, llvm::StringRef Input);

// Candidates for `Text` with the cursor at its end, as ReplCodeCompleter produces them
// (sorted, without duplicates). `Prefix` receives the part of the text being completed.
std::vector<std::string> completeText(Session &S, llvm::StringRef Text, std::string &Prefix, llvm::Error &Err);

// The list completer handed to llvm::LineEditor (Tab).
struct TabCompleter {
  Session &S;
  std::vector<llvm::LineEditor::Completion> operator()(llvm::StringRef Buffer, size_t Pos) const;
};

} // namespace xrepl

#endif
