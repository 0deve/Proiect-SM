#include "wifi_server.h"
#include "secrets.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// pini hardware (duplicate din main.c)
#define RELAY_PIN      16
#define LED_PIN_AUTO   17
#define LED_PIN_MANUAL 20

// variabile globale din main.c
extern volatile bool auto_mode;
extern volatile bool relay_state;
extern volatile float ultima_temperatura;
extern volatile bool temperatura_valida;
extern volatile uint16_t ultima_lumina;
extern volatile bool is_daytime;
extern volatile float temp_threshold_day;
extern volatile float temp_threshold_night;
extern volatile uint16_t light_threshold;
extern volatile bool oled_needs_update;

extern mutex_t state_mutex;

static char ip_str[16] = "0.0.0.0";
static struct tcp_pcb *server_pcb = NULL;

// parsare query string (cu verificare delimitator pentru a evita false match)

static bool parse_query_int(const char *query, const char *key, int *value) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *pos = strstr(query, search);
    while (pos) {
        if (pos == query || *(pos - 1) == '&' || *(pos - 1) == '?' || *(pos - 1) == '\n') {
            *value = atoi(pos + strlen(search));
            return true;
        }
        pos = strstr(pos + 1, search);
    }
    return false;
}

static bool parse_query_float(const char *query, const char *key, float *value) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *pos = strstr(query, search);
    while (pos) {
        if (pos == query || *(pos - 1) == '&' || *(pos - 1) == '?' || *(pos - 1) == '\n') {
            *value = (float)atof(pos + strlen(search));
            return true;
        }
        pos = strstr(pos + 1, search);
    }
    return false;
}

// cauta parametrii in query string SAU in body-ul POST
static const char* find_params(const char *request) {
    // intai cautam query string din URL
    const char *query = strchr(request, '?');
    if (query) return query;
    // apoi cautam in body (dupa \r\n\r\n)
    const char *body = strstr(request, "\r\n\r\n");
    if (body) return body + 4;
    return NULL;
}

// trimite raspuns http cu json (cu CORS headers)

static void send_response(struct tcp_pcb *tpcb, const char *json) {
    char response[800];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n%s",
        (int)strlen(json), json);

    tcp_write(tpcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

// inchide conexiunea dupa trimitere (cu fallback la tcp_abort)

static err_t server_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    err_t err = tcp_close(tpcb);
    if (err != ERR_OK) {
        tcp_abort(tpcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

// procesare cerere http

static void handle_request(struct tcp_pcb *tpcb, const char *request) {
    char json[400];

    if (strncmp(request, "GET /status", 11) == 0) {
        mutex_enter_blocking(&state_mutex);
        snprintf(json, sizeof(json),
            "{\"temperature\":%.1f,\"light\":%u,\"relay\":%s,"
            "\"auto_mode\":%s,\"is_daytime\":%s,"
            "\"threshold_day\":%.1f,\"threshold_night\":%.1f,"
            "\"light_threshold\":%u}",
            temperatura_valida ? ultima_temperatura : 0.0f,
            ultima_lumina,
            relay_state ? "true" : "false",
            auto_mode ? "true" : "false",
            is_daytime ? "true" : "false",
            temp_threshold_day,
            temp_threshold_night,
            light_threshold);
        mutex_exit(&state_mutex);
    }
    else if (strncmp(request, "POST /relay", 11) == 0) {
        mutex_enter_blocking(&state_mutex);
        if (!auto_mode) {
            int state = -1;
            const char *params = find_params(request);
            if (params && parse_query_int(params, "state", &state)) {
                relay_state = (state == 1);
                gpio_put(RELAY_PIN, relay_state ? 0 : 1);
                printf("[HTTP] Releu -> %s\n", relay_state ? "ON" : "OFF");
                snprintf(json, sizeof(json),
                    "{\"success\":true,\"state\":%s}",
                    relay_state ? "true" : "false");
                oled_needs_update = true;
            } else {
                snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"missing state param\"}");
            }
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"auto mode active\"}");
        }
        mutex_exit(&state_mutex);
    }
    else if (strncmp(request, "POST /mode", 10) == 0) {
        const char *params = find_params(request);
        mutex_enter_blocking(&state_mutex);
        if (params && strstr(params, "mode=auto")) {
            auto_mode = true;
            gpio_put(LED_PIN_MANUAL, 0);
            gpio_put(LED_PIN_AUTO, 1);
            printf("[HTTP] Mod -> AUTO\n");
            snprintf(json, sizeof(json), "{\"success\":true,\"mode\":\"auto\"}");
            oled_needs_update = true;
        } else if (params && strstr(params, "mode=manual")) {
            auto_mode = false;
            relay_state = false;
            gpio_put(RELAY_PIN, 1);
            gpio_put(LED_PIN_MANUAL, 1);
            gpio_put(LED_PIN_AUTO, 0);
            temperatura_valida = false;
            printf("[HTTP] Mod -> MANUAL\n");
            snprintf(json, sizeof(json), "{\"success\":true,\"mode\":\"manual\"}");
            oled_needs_update = true;
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"invalid mode\"}");
        }
        mutex_exit(&state_mutex);
    }
    else if (strncmp(request, "POST /threshold", 15) == 0) {
        const char *params = find_params(request);
        mutex_enter_blocking(&state_mutex);
        if (params) {
            float day_val, night_val;
            int light_val;
            if (parse_query_float(params, "day", &day_val))
                if (day_val >= 5.0f && day_val <= 45.0f) temp_threshold_day = day_val;
            if (parse_query_float(params, "night", &night_val))
                if (night_val >= 5.0f && night_val <= 45.0f) temp_threshold_night = night_val;
            if (parse_query_int(params, "light", &light_val))
                if (light_val >= 0 && light_val <= 4095) light_threshold = (uint16_t)light_val;
            printf("[HTTP] Praguri -> ZI:%.1f NOAPTE:%.1f LUMINA:%u\n",
                   temp_threshold_day, temp_threshold_night, light_threshold);
            snprintf(json, sizeof(json),
                "{\"success\":true,\"threshold_day\":%.1f,\"threshold_night\":%.1f,\"light_threshold\":%u}",
                temp_threshold_day, temp_threshold_night, light_threshold);
            oled_needs_update = true;
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"missing params\"}");
        }
        mutex_exit(&state_mutex);
    }
    else if (strncmp(request, "GET /ping", 9) == 0) {
        snprintf(json, sizeof(json), "{\"alive\":true}");
    }
    else if (strncmp(request, "OPTIONS ", 8) == 0) {
        // raspuns CORS preflight
        const char *cors =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        tcp_sent(tpcb, server_sent_cb);
        tcp_write(tpcb, cors, strlen(cors), TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        return;
    }
    else {
        snprintf(json, sizeof(json), "{\"error\":\"unknown endpoint\"}");
    }

    tcp_sent(tpcb, server_sent_cb);
    send_response(tpcb, json);
}

// callback: date primite pe conexiune tcp

static err_t server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    char request[512];
    int len = pbuf_copy_partial(p, request, sizeof(request) - 1, 0);
    request[len] = '\0';

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    handle_request(tpcb, request);

    return ERR_OK;
}

// callback: eroare pe conexiune (clientul s-a deconectat brusc)

static void server_err_cb(void *arg, err_t err) {
    printf("[HTTP] Conexiune eroare: %d\n", err);
    // PCB-ul a fost deja eliberat de lwIP
}

// callback: conexiune noua acceptata

static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, server_recv_cb);
    tcp_err(newpcb, server_err_cb);
    return ERR_OK;
}

// porneste serverul tcp pe portul 80

static bool wifi_server_start(void) {
    server_pcb = tcp_new();
    if (!server_pcb) {
        printf("[WIFI] Eroare: tcp_new() a esuat\n");
        return false;
    }

    err_t err = tcp_bind(server_pcb, IP_ADDR_ANY, 80);
    if (err != ERR_OK) {
        printf("[WIFI] Eroare: tcp_bind() a esuat (%d)\n", err);
        return false;
    }

    server_pcb = tcp_listen(server_pcb);
    if (!server_pcb) {
        printf("[WIFI] Eroare: tcp_listen() a esuat\n");
        return false;
    }

    tcp_accept(server_pcb, server_accept_cb);
    printf("[WIFI] Server HTTP pornit pe portul 80\n");
    return true;
}

// conectare wifi + pornire server

bool wifi_server_init(void) {
    if (cyw43_arch_init_with_country(CYW43_COUNTRY('R', 'O', 0))) {
        printf("[WIFI] Eroare: cyw43_arch_init a esuat!\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1));

    printf("[WIFI] Conectare la '%s'...\n", WIFI_SSID);
    stdio_flush();

    #define WIFI_MAX_RETRIES 3
    #define WIFI_TIMEOUT_MS 15000

    bool connected = false;

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES && !connected; attempt++) {
        printf("[WIFI] Incercare %d/%d...\n", attempt, WIFI_MAX_RETRIES);
        stdio_flush();

        // conectare sincrona cu timeout - mult mai stabila pe RP2350
        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, WIFI_TIMEOUT_MS);

        if (err == 0) {
            // verificam daca avem IP de la DHCP
            if (netif_list) {
                const ip4_addr_t *addr = netif_ip4_addr(netif_list);
                if (addr && addr->addr != 0) {
                    connected = true;
                    printf("[WIFI] Conectat! IP detectat.\n");
                    stdio_flush();
                }
            }

            // daca wifi e ok dar nu avem IP, asteptam DHCP inca putin
            if (!connected) {
                printf("[WIFI] WiFi asociat, asteptam DHCP...\n");
                stdio_flush();
                for (int w = 0; w < 100 && !connected; w++) {
                    cyw43_arch_poll();
                    sleep_ms(100);
                    if (netif_list) {
                        const ip4_addr_t *addr = netif_ip4_addr(netif_list);
                        if (addr && addr->addr != 0) {
                            connected = true;
                            printf("[WIFI] DHCP OK!\n");
                            stdio_flush();
                        }
                    }
                }
            }

            // fallback IP static daca DHCP nu a mers
            if (!connected && netif_list) {
                printf("[WIFI] DHCP esuat - setam IP static\n");
                stdio_flush();
                dhcp_release_and_stop(netif_list);
                sleep_ms(100);

                ip4_addr_t ip, mask, gw;
                IP4_ADDR(&ip, 192, 168, 43, 100);
                IP4_ADDR(&mask, 255, 255, 255, 0);
                IP4_ADDR(&gw, 192, 168, 43, 1);
                netif_set_addr(netif_list, &ip, &mask, &gw);
                netif_set_up(netif_list);

                // polling scurt sa aplice adresa
                for (int p = 0; p < 10; p++) {
                    cyw43_arch_poll();
                    sleep_ms(50);
                }

                const ip4_addr_t *check = netif_ip4_addr(netif_list);
                if (check && check->addr != 0) {
                    connected = true;
                    printf("[WIFI] IP static setat: %s\n", ip4addr_ntoa(check));
                    stdio_flush();
                }
            }
        } else {
            printf("[WIFI] Incercarea %d esuata (err=%d)\n", attempt, err);
            stdio_flush();
        }

        if (!connected && attempt < WIFI_MAX_RETRIES) {
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            sleep_ms(2000);
        }
    }

    if (!connected) {
        printf("[WIFI] Toate incercarile au esuat!\n");
        stdio_flush();
        return false;
    }

    if (netif_list) {
        const ip4_addr_t *addr = netif_ip4_addr(netif_list);
        if (addr) {
            snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(addr));
        }
    }

    printf("[WIFI] Conectat! IP: %s\n", ip_str);
    stdio_flush();
    return wifi_server_start();
}

void wifi_server_poll(void) {
    cyw43_arch_poll();
}

const char* wifi_server_get_ip(void) {
    return ip_str;
}
