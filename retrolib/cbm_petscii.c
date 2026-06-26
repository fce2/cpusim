#include "cbm_petscii.h"
#include <string.h>

void cbm_ascii_to_petscii(const char *s, uint8_t *out, int length)
{
	int i;
	for (i = 0; s[i] && i < length; i++)
		out[i] = (uint8_t)((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i] & 0x7F);
	for (; i < length; i++) out[i] = CBM_PAD_CHAR;
}

int cbm_petscii_to_ascii(const uint8_t *data, int len, char *out, int out_cap)
{
	int i, j = 0;
	for (i = 0; i < len && j < out_cap - 1; i++) {
		uint8_t b = data[i];
		if (b == 0x00) break;
		if (b == CBM_PAD_CHAR) out[j++] = ' ';
		else if (b >= 0x41 && b <= 0x5A) out[j++] = (char)b;			/* A-Z */
		else if (b >= 0xC1 && b <= 0xDA) out[j++] = (char)(b - 0x80);	/* lowercase → A-Z */
		else if (b >= 0x20 && b <= 0x5F) out[j++] = (char)b;			/* space, digits, punct */
		else if (b >= 0x01 && b <= 0x1F) out[j++] = (char)(b + 0x40);	/* reverse-video A-Z, etc. */
		else if (b >= 0x80 && b <= 0x9F) out[j++] = (char)(b - 0x80 + 0x40); /* reverse + ctrl */
		else if (b >= 0x60 && b <= 0x7F) out[j++] = '.';				/* PETSCII graphics */
		else if (b >= 0xA0 && b <= 0xBF) out[j++] = '.';				/* shifted PETSCII graphics */
		else if (b >= 0xE0 && b <= 0xFF) out[j++] = '.';				/* PETSCII graphics (high) */
		else out[j++] = '.';
	}
	while (j > 0 && out[j - 1] == ' ') j--;
	out[j] = '\0';
	return j;
}

int c64_screen_line(const uint8_t *screen, int row, char *out, int out_cap)
{
	return cbm_petscii_to_ascii(screen + row * 40, 40, out, out_cap);
}
