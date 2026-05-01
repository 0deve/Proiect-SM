#include "pico/stdlib.h"
#include "ds18b20.h"

// Driver DS18B20 - Protocol 1-Wire
// Cablare:
//   VCC  -> 3.3V
//   GND  -> GND
//   DATA -> GP15 + rezistenta 4.7kΩ intre DATA si 3.3V

static bool onewire_reset(uint pin) {
    // Tragem linia LOW pentru 480us (semnalul de reset)
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(480);

    // Eliberam linia (pull-up o duce la HIGH) si asteptam raspunsul senzorului
    gpio_set_dir(pin, GPIO_IN);
    sleep_us(70);

    // Citim linia: daca senzorul e prezent, va trage linia LOW
    bool presence = !gpio_get(pin);
    sleep_us(410);

    return presence;
}

static void onewire_write_bit(uint pin, bool bit) {
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0); // Incepem cu linia LOW

    if (bit) {
        sleep_us(6);
        gpio_set_dir(pin, GPIO_IN);
        sleep_us(64);
    } else {
        sleep_us(60);
        gpio_set_dir(pin, GPIO_IN);
        sleep_us(10);
    }
}

static bool onewire_read_bit(uint pin) {
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(6);            

    gpio_set_dir(pin, GPIO_IN);
    sleep_us(9);            

    bool bit = gpio_get(pin); 
    sleep_us(55);           

    return bit;
}

static void onewire_write_byte(uint pin, uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        onewire_write_bit(pin, byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t onewire_read_byte(uint pin) {
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        if (onewire_read_bit(pin)) {
            byte |= (1 << i);
        }
    }
    return byte;
}


void ds18b20_init(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
}

bool ds18b20_read_temperature(uint pin, float *temperature) {
    if (!onewire_reset(pin)) {
        return false;
    }

    // SKIP ROM
    onewire_write_byte(pin, 0xCC);

    // Convert T, pornim citirea
    onewire_write_byte(pin, 0x44);

    // Asteptam conversia
    sleep_ms(750);

    if (!onewire_reset(pin)) {
        return false;
    }

    onewire_write_byte(pin, 0xCC);

    // READ SCRATCHPAD: citim continutul
    onewire_write_byte(pin, 0xBE);

    // Citim primii doi bytes, adica temperatura
    // Byte 0 = LSB, Byte 1 = MSB
    uint8_t temp_lsb = onewire_read_byte(pin);
    uint8_t temp_msb = onewire_read_byte(pin);

    int16_t raw_temp = (temp_msb << 8) | temp_lsb;

    *temperature = raw_temp / 16.0f;

    return true;
}
