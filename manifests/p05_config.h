// Part 5.6 -- a layout shared by the host tool and the interpreted code.
// Both sides #include this file, so both compile the same struct.
#ifndef P05_CONFIG_H
#define P05_CONFIG_H

struct Config {
  int verbosity;
  double scale;
  char name[16];
  int limits[4];
};

// A callback type: the host hands the interpreter one of these.
typedef void (*ReportFn)(const char *what, int value);

#endif
