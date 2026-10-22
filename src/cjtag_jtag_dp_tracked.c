/** The copy of the two-pin JTAG path for the formats whose frames depend on the TAP state, OScan2,
 * OScan5 and OScan6.
 *
 * Same source as src/cjtag_jtag_dp.c, ARM's JTAG_DP.c with the pin layer shadowed, driving
 * cJTAG_FrameTap, which picks each frame's shape from the format and the TAP state. One paced copy:
 * the generated frames are the fast path, and this one only has to be right.
 */
#define CJTAG_FRAME cJTAG_FrameTap
#define CJTAG_NAME(name) cjtagt_##name
#define CJTAG_BLOCK_ATTRS

#include "cjtag_jtag_dp.c"
