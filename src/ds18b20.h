#ifndef DS18B20_H
#define DS18B20_H

#include <stdbool.h>

void ds18b20_init(uint pin);
bool ds18b20_read_temperature(uint pin, float *temperature);

#endif
