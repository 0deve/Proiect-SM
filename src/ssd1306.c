#include "ssd1306.h"
#include "font5x8.h"
#include "pico/stdlib.h"
#include <string.h>

// Comenzi SSD1306
#define SSD1306_SET_LOW_COLUMN        0x00
#define SSD1306_SET_HIGH_COLUMN       0x10
#define SSD1306_SET_MEMORY_MODE       0x20
#define SSD1306_SET_COL_ADDR          0x21
#define SSD1306_SET_PAGE_ADDR         0x22
#define SSD1306_SET_START_LINE        0x40
#define SSD1306_SET_CONTRAST          0x81
#define SSD1306_SET_CHARGE_PUMP       0x8D
#define SSD1306_SET_SEG_REMAP         0xA1
#define SSD1306_SET_ENTIRE_ON         0xA4
#define SSD1306_SET_NORMAL_DISPLAY    0xA6
#define SSD1306_SET_MUX_RATIO         0xA8
#define SSD1306_SET_DISPLAY_OFF       0xAE
#define SSD1306_SET_DISPLAY_ON        0xAF
#define SSD1306_SET_COM_SCAN_DEC      0xC8
#define SSD1306_SET_DISPLAY_OFFSET    0xD3
#define SSD1306_SET_DISPLAY_CLK_DIV   0xD5
#define SSD1306_SET_PRECHARGE         0xD9
#define SSD1306_SET_COM_PIN_CFG       0xDA
#define SSD1306_SET_VCOMH_DESELECT    0xDB

// Trimite o comanda catre SSD1306
static void ssd1306_cmd(ssd1306_t *disp, uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd}; // Co=0, D/C#=0 (comanda)
    i2c_write_blocking(disp->i2c, SSD1306_I2C_ADDR, buf, 2, false);
}

void ssd1306_init(ssd1306_t *disp, i2c_inst_t *i2c, uint sda_pin, uint scl_pin) {
    disp->i2c = i2c;

    // Configurare I2C la 400kHz (fast mode)
    i2c_init(i2c, 400 * 1000);

    // Configurare pini I2C
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);

    // Secventa de initializare SSD1306 pentru 128x64
    sleep_ms(100); // Asteptam power-up

    ssd1306_cmd(disp, SSD1306_SET_DISPLAY_OFF);

    ssd1306_cmd(disp, SSD1306_SET_DISPLAY_CLK_DIV);
    ssd1306_cmd(disp, 0x80); // Raport de ceas default

    ssd1306_cmd(disp, SSD1306_SET_MUX_RATIO);
    ssd1306_cmd(disp, SSD1306_HEIGHT - 1); // 63

    ssd1306_cmd(disp, SSD1306_SET_DISPLAY_OFFSET);
    ssd1306_cmd(disp, 0x00); // Fara offset

    ssd1306_cmd(disp, SSD1306_SET_START_LINE | 0x00); // Linia de start 0

    ssd1306_cmd(disp, SSD1306_SET_CHARGE_PUMP);
    ssd1306_cmd(disp, 0x14); // Activam charge pump (necesar pentru alimentare 3.3V)

    ssd1306_cmd(disp, SSD1306_SET_MEMORY_MODE);
    ssd1306_cmd(disp, 0x00); // Horizontal addressing mode

    ssd1306_cmd(disp, SSD1306_SET_SEG_REMAP);       // Segment remap (coloana 127 = SEG0)
    ssd1306_cmd(disp, SSD1306_SET_COM_SCAN_DEC);     // COM scan descrescator

    ssd1306_cmd(disp, SSD1306_SET_COM_PIN_CFG);
    ssd1306_cmd(disp, 0x12); // Configuratie COM alternativa pentru 128x64

    ssd1306_cmd(disp, SSD1306_SET_CONTRAST);
    ssd1306_cmd(disp, 0xCF); // Contrast ridicat

    ssd1306_cmd(disp, SSD1306_SET_PRECHARGE);
    ssd1306_cmd(disp, 0xF1); // Pre-charge period

    ssd1306_cmd(disp, SSD1306_SET_VCOMH_DESELECT);
    ssd1306_cmd(disp, 0x40); // VCOMH deselect level

    ssd1306_cmd(disp, SSD1306_SET_ENTIRE_ON);        // Output urmeaza RAM-ul
    ssd1306_cmd(disp, SSD1306_SET_NORMAL_DISPLAY);    // Display normal (nu inversat)

    // Stergem framebuffer-ul si afisam
    ssd1306_clear(disp);
    ssd1306_render(disp);

    ssd1306_cmd(disp, SSD1306_SET_DISPLAY_ON);        // Pornim display-ul
}

void ssd1306_clear(ssd1306_t *disp) {
    memset(disp->buffer, 0, sizeof(disp->buffer));
}

void ssd1306_render(ssd1306_t *disp) {
    // Setam zona de scriere: intregul display
    ssd1306_cmd(disp, SSD1306_SET_COL_ADDR);
    ssd1306_cmd(disp, 0);                     // Coloana start
    ssd1306_cmd(disp, SSD1306_WIDTH - 1);     // Coloana stop

    ssd1306_cmd(disp, SSD1306_SET_PAGE_ADDR);
    ssd1306_cmd(disp, 0);                                   // Pagina start
    ssd1306_cmd(disp, (SSD1306_HEIGHT / 8) - 1);           // Pagina stop

    // Trimitem framebuffer-ul ca date
    // Formatul I2C: primul byte = 0x40 (Co=0, D/C#=1 = data)
    // Trimitem in blocuri de 32 bytes (limita I2C buffer)
    size_t buf_len = SSD1306_WIDTH * SSD1306_HEIGHT / 8;
    uint8_t temp[33]; // 1 byte control + 32 bytes date
    temp[0] = 0x40;   // Control byte: date

    for (size_t i = 0; i < buf_len; i += 32) {
        size_t chunk = (buf_len - i < 32) ? (buf_len - i) : 32;
        memcpy(temp + 1, disp->buffer + i, chunk);
        i2c_write_blocking(disp->i2c, SSD1306_I2C_ADDR, temp, chunk + 1, false);
    }
}

void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool color) {
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return;

    // In horizontal addressing mode:
    // buffer[page * width + x], unde page = y / 8
    // Bitul corespunzator in byte este y % 8
    int page = y / 8;
    int bit = y % 8;
    int idx = page * SSD1306_WIDTH + x;

    if (color) {
        disp->buffer[idx] |= (1 << bit);
    } else {
        disp->buffer[idx] &= ~(1 << bit);
    }
}

void ssd1306_char(ssd1306_t *disp, int x, int y, char c) {
    if (c < 32 || c > 127) c = '?'; // Caracter invalid -> '?'

    const uint8_t *glyph = font5x8[c - 32];

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 8; row++) {
            ssd1306_pixel(disp, x + col, y + row, (line >> row) & 1);
        }
    }
    // Spatiu de 1 pixel intre caractere
    for (int row = 0; row < 8; row++) {
        ssd1306_pixel(disp, x + 5, y + row, false);
    }
}

void ssd1306_string(ssd1306_t *disp, int x, int y, const char *str) {
    int cursor_x = x;
    while (*str) {
        if (cursor_x + 6 > SSD1306_WIDTH) break; // Nu mai incape pe linie
        ssd1306_char(disp, cursor_x, y, *str);
        cursor_x += 6; // 5 pixeli caracter + 1 pixel spatiu
        str++;
    }
}

void ssd1306_hline(ssd1306_t *disp, int x, int y, int w, bool color) {
    for (int i = 0; i < w; i++) {
        ssd1306_pixel(disp, x + i, y, color);
    }
}

void ssd1306_rect(ssd1306_t *disp, int x, int y, int w, int h, bool color) {
    // Linie sus
    ssd1306_hline(disp, x, y, w, color);
    // Linie jos
    ssd1306_hline(disp, x, y + h - 1, w, color);
    // Linii laterale
    for (int i = 0; i < h; i++) {
        ssd1306_pixel(disp, x, y + i, color);
        ssd1306_pixel(disp, x + w - 1, y + i, color);
    }
}
