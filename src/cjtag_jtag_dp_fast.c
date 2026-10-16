/** The delay-free copy of the OScan1 JTAG path.
 *
 * Same source as src/cjtag_jtag_dp.c, which is itself ARM's JTAG_DP.c with the pin layer
 * shadowed. Only two things differ: the frame it drives does not pace, and its entry points are
 * named apart so src/cjtag_dp.c can pick between them once per call.
 *
 * The choice has to be made above the frame. Making it inside costs a cycle in the paced copy,
 * which is enough to take a 10 MHz request over the clock the host asked for -- see the note in
 * src/cjtag.c, and OPTIMISATIONS.md for what it is worth.
 */
#define CJTAG_FRAME cJTAG_FrameFast
#define CJTAG_NAME(name) cjtagf_##name

#include "cjtag_jtag_dp.c"
