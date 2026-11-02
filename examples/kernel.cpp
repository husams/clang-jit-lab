// C linkage gives the host a stable name; int(int) is the entry interface it validates.
extern "C" int run(int x) { return x * 7; }
