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

static char ip_str[16] = "0.0.0.0";
static struct tcp_pcb *server_pcb = NULL;

// parsare query string

static bool parse_query_int(const char *query, const char *key, int *value) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *pos = strstr(query, search);
    if (pos) {
        *value = atoi(pos + strlen(search));
        return true;
    }
    return false;
}

static bool parse_query_float(const char *query, const char *key, float *value) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *pos = strstr(query, search);
    if (pos) {
        *value = (float)atof(pos + strlen(search));
        return true;
    }
    return false;
}

// trimite raspuns http cu json

static void send_response(struct tcp_pcb *tpcb, const char *json) {
    char response[600];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n%s",
        (int)strlen(json), json);

    tcp_write(tpcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

// inchide conexiunea dupa trimitere

static err_t server_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    tcp_close(tpcb);
    return ERR_OK;
}

// procesare cerere http

static void handle_request(struct tcp_pcb *tpcb, const char *request) {
    char json[400];

    if (strncmp(request, "GET /status", 11) == 0) {
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
    }
    else if (strncmp(request, "POST /relay", 11) == 0) {
        if (!auto_mode) {
            int state = -1;
            const char *query = strchr(request, '?');
            if (query && parse_query_int(query, "state", &state)) {
                relay_state = (state == 1);
                gpio_put(RELAY_PIN, relay_state ? 0 : 1);
                printf("[HTTP] Releu -> %s\n", relay_state ? "ON" : "OFF");
                snprintf(json, sizeof(json),
                    "{\"success\":true,\"state\":%s}",
                    relay_state ? "true" : "false");
            } else {
                snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"missing state param\"}");
            }
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"auto mode active\"}");
        }
    }
    else if (strncmp(request, "POST /mode", 10) == 0) {
        const char *query = strchr(request, '?');
        if (query && strstr(query, "mode=auto")) {
            auto_mode = true;
            gpio_put(LED_PIN_MANUAL, 0);
            gpio_put(LED_PIN_AUTO, 1);
            printf("[HTTP] Mod -> AUTO\n");
            snprintf(json, sizeof(json), "{\"success\":true,\"mode\":\"auto\"}");
        } else if (query && strstr(query, "mode=manual")) {
            auto_mode = false;
            relay_state = false;
            gpio_put(RELAY_PIN, 1);
            gpio_put(LED_PIN_MANUAL, 1);
            gpio_put(LED_PIN_AUTO, 0);
            temperatura_valida = false;
            printf("[HTTP] Mod -> MANUAL\n");
            snprintf(json, sizeof(json), "{\"success\":true,\"mode\":\"manual\"}");
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"invalid mode\"}");
        }
    }
    else if (strncmp(request, "POST /threshold", 15) == 0) {
        const char *query = strchr(request, '?');
        if (query) {
            float day_val, night_val;
            int light_val;
            if (parse_query_float(query, "day", &day_val))
                temp_threshold_day = day_val;
            if (parse_query_float(query, "night", &night_val))
                temp_threshold_night = night_val;
            if (parse_query_int(query, "light", &light_val))
                light_threshold = (uint16_t)light_val;
            printf("[HTTP] Praguri -> ZI:%.1f NOAPTE:%.1f LUMINA:%u\n",
                   temp_threshold_day, temp_threshold_night, light_threshold);
            snprintf(json, sizeof(json),
                "{\"success\":true,\"threshold_day\":%.1f,\"threshold_night\":%.1f,\"light_threshold\":%u}",
                temp_threshold_day, temp_threshold_night, light_threshold);
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"missing params\"}");
        }
    }
    else if (strncmp(request, "GET /ping", 9) == 0) {
        snprintf(json, sizeof(json), "{\"alive\":true}");
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

    char request[300];
    int len = pbuf_copy_partial(p, request, sizeof(request) - 1, 0);
    request[len] = '\0';

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    handle_request(tpcb, request);

    return ERR_OK;
}

// callback: conexiune noua acceptata

static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, server_recv_cb);
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

    #define WIFI_MAX_RETRIES 3
    #define WIFI_TIMEOUT_SECONDS 30

    bool connected = false;

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES && !connected; attempt++) {
        printf("[WIFI] Incercare %d/%d...\n", attempt, WIFI_MAX_RETRIES);

        cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

        bool dhcp_restarted = false;

        for (int s = 0; s < WIFI_TIMEOUT_SECONDS * 10 && !connected; s++) {
            // in poll mode trebuie sa procesam manual evenimentele wifi/lwip
            for (int p = 0; p < 100; p++) {
                cyw43_arch_poll();
                sleep_ms(1);
            }

            // verificam ip
            struct netif *nif = netif_list;
            if (nif != NULL) {
                const ip4_addr_t *addr = netif_ip4_addr(nif);
                if (addr && addr->addr != 0) {
                    connected = true;
                    printf("[WIFI] Conectat! IP detectat.\n");
                    break;
                }
            }

            // restart dhcp dupa 5s
            if (s == 50 && !dhcp_restarted && netif_list != NULL) {
                int ws = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
                if (ws >= CYW43_LINK_JOIN) {
                    printf("[WIFI] wifi ok dar fara ip - restart dhcp\n");
                    netif_set_up(netif_list);
                    netif_set_link_up(netif_list);
                    dhcp_release_and_stop(netif_list);
                    for (int p = 0; p < 200; p++) { cyw43_arch_poll(); sleep_ms(1); }
                    dhcp_start(netif_list);
                    dhcp_restarted = true;
                }
            }

            // ip static fallback dupa 10s
            if (s == 100 && !connected && netif_list != NULL) {
                printf("[WIFI] dhcp esuat - ip static\n");
                dhcp_release_and_stop(netif_list);
                ip4_addr_t ip, mask, gw;
                IP4_ADDR(&ip, 192, 168, 43, 100);
                IP4_ADDR(&mask, 255, 255, 255, 0);
                IP4_ADDR(&gw, 192, 168, 43, 1);
                netif_set_addr(netif_list, &ip, &mask, &gw);
                netif_set_up(netif_list);

                const ip4_addr_t *check = netif_ip4_addr(netif_list);
                if (check && check->addr != 0) {
                    connected = true;
                    printf("[WIFI] ip static: %s\n", ip4addr_ntoa(check));
                }
            }

            // log la fiecare secunda primele 10s, apoi la 2s
            bool should_log = (s < 100) ? (s % 10 == 0) : (s % 20 == 0);
            if (should_log) {
                int ws = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
                int ts = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                printf("[WIFI] t=%ds wifi=%d tcpip=%d netif=%s\n",
                       s / 10, ws, ts, netif_list ? "ok" : "NULL");
            }

            if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) < 0) {
                printf("[WIFI] Eroare fatala wifi\n");
                break;
            }
        }

        if (!connected) {
            printf("[WIFI] Incercarea %d esuata.\n", attempt);
            if (attempt < WIFI_MAX_RETRIES) {
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                sleep_ms(2000);
            }
        }
    }

    if (!connected) {
        printf("[WIFI] Toate incercarile au esuat!\n");
        return false;
    }

    if (netif_list) {
        const ip4_addr_t *addr = netif_ip4_addr(netif_list);
        if (addr) {
            snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(addr));
        }
    }

    printf("[WIFI] Conectat! IP: %s\n", ip_str);
    return wifi_server_start();
}

void wifi_server_poll(void) {
    cyw43_arch_poll();
}

const char* wifi_server_get_ip(void) {
    return ip_str;
}
