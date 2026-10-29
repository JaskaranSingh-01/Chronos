// Lua's ltablib.c with a fixed pivot for table.sort. Lua re-randomizes a sort's pivot from the clock when a partition
// is unbalanced, so elements that compare equal could end in another order from one run to the next.
#define l_randomizePivot() 0u
#include "ltablib.c"
