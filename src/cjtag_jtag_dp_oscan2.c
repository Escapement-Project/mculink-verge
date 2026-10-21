/** The OScan2 copy of the two-pin JTAG path.
 *
 * Same source as src/cjtag_jtag_dp.c, ARM's JTAG_DP.c with the pin layer shadowed, driving
 * cJTAG_Frame2, which picks each frame's shape from the TAP state. One paced copy: the generated
 * frames are the fast path, and this one only has to be right.
 */
#define CJTAG_FRAME cJTAG_Frame2
#define CJTAG_NAME(name) cjtag2_##name
#define CJTAG_BLOCK_ATTRS

#include "cjtag_jtag_dp.c"
