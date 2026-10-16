#ifndef SER_NUM_H
#define SER_NUM_H

/** The probe's serial number as a NUL-terminated string.
 *
 * It has to be stable and distinct per unit: probe-rs and every other host tool address a probe by
 * this string, and two MCU-Links on one bench are indistinguishable without it.
 */
char *GetSerialNum(void);

#endif
