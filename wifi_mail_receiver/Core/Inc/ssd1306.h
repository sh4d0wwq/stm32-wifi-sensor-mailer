#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>

int SSD1306_Init(void);
void SSD1306_Clear(void);
void SSD1306_DrawString(int x, int page, const char *s);
void SSD1306_Update(void);

#endif
