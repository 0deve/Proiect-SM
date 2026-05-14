#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

// Structura cu pointeri catre starea sistemului (shared cu main.c)
typedef struct {
    volatile float    *temperature;
    volatile bool     *temperatura_valida;
    volatile uint16_t *light;
    volatile bool     *relay_state;
    volatile bool     *auto_mode;
    volatile bool     *is_daytime;
    volatile float    *threshold_day;
    volatile float    *threshold_night;
    volatile uint16_t *light_threshold;

    // Callback-uri pentru actiuni din HTTP
    void (*set_relay)(bool on);
    void (*set_mode)(bool auto_on);
    void (*update_display)(void);
} http_system_state_t;

// Porneste serverul HTTP pe portul 80
// Returneaza true daca serverul a pornit cu succes
bool http_server_init(http_system_state_t *state);

#endif
