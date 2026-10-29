// Lua's lstate.c with a fixed string-hash seed. Lua seeds each state's string hashes from the clock and an address,
// which changes the order `pairs` visits a table's string keys from one run to the next; a script's run must repeat.
#define luai_makeseed(L) 0u
#include "lstate.c"
