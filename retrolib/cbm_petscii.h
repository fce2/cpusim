/* cbm_petscii.h - PETSCII character conversion
   Convert between PETSCII (Commodore character set) and ASCII. */

#ifndef RETROLIB_CBM_PETSCII_H
#define RETROLIB_CBM_PETSCII_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CBM_FILENAME_LEN	16		/* Standard CBM filename length */
#define CBM_PAD_CHAR		0xA0		/* PETSCII padding character (shifted space) */

/* Convert ASCII string to PETSCII. Uppercases a-z, strips high bit, pads with 0xA0. */
void cbm_ascii_to_petscii(const char *s, uint8_t *out, int length);

/* Convert PETSCII data to ASCII. 0xA0->space, uppercase, hex for unknowns. */
int cbm_petscii_to_ascii(const uint8_t *data, int len, char *out, int out_cap);

/* Get a C64 screen line (truncated, no trailing spaces). Returns line length, 0 if empty. */
int c64_screen_line(const uint8_t *screen, int row, char *out, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* RETROLIB_CBM_PETSCII_H */