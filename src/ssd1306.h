#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"

// Dimensiunile display-ului OLED 0.96" (128x64)
#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 64

// Adresa I2C default a SSD1306
#define SSD1306_I2C_ADDR 0x3C

// Structura pentru display
typedef struct {
    i2c_inst_t *i2c;
    uint8_t buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8]; // Framebuffer: 1 bit per pixel
} ssd1306_t;

// Initializare display OLED
// i2c: instanta I2C
// sda_pin: GPIO pin pentru SDA
// scl_pin: GPIO pin pentru SCL
void ssd1306_init(ssd1306_t *disp, i2c_inst_t *i2c, uint sda_pin, uint scl_pin);

// Sterge framebuffer-ul (toate pixelii OFF)
void ssd1306_clear(ssd1306_t *disp);

// Trimite framebuffer-ul la display (render)
void ssd1306_render(ssd1306_t *disp);

// Deseneaza un pixel la coordonatele (x, y)
// color: true = pixel aprins, false = pixel stins
void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool color);

// Scrie un caracter la pozitia (x, y) cu font 5x8
void ssd1306_char(ssd1306_t *disp, int x, int y, char c);

// Scrie un string incepand de la pozitia (x, y)
void ssd1306_string(ssd1306_t *disp, int x, int y, const char *str);

// Deseneaza o linie orizontala
void ssd1306_hline(ssd1306_t *disp, int x, int y, int w, bool color);

// Deseneaza un dreptunghi (doar contur)
void ssd1306_rect(ssd1306_t *disp, int x, int y, int w, int h, bool color);

#endif // SSD1306_H
