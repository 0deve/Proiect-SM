#include "http_server.h"
#include "lwip/tcp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HTTP_PORT 80

static http_system_state_t *sys_state = NULL;

// ── Trimitere răspuns HTTP ──
static err_t send_response(struct tcp_pcb *pcb, const char *status, const char *content_type, const char *body) {
    char header[256];
    int body_len = strlen(body);
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len);
    
    tcp_write(pcb, header, hdr_len, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, body, body_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

// ── Router HTTP ──
static void handle_request(struct tcp_pcb *pcb, const char *request) {
    if (strstr(request, "GET /ping ")) {
        send_response(pcb, "200 OK", "application/json", "{\"alive\":true}");
    } else if (strstr(request, "GET /status ")) {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"temperature\":%.1f,"
            "\"light\":%u,"
            "\"relay\":%s,"
            "\"auto_mode\":%s,"
            "\"is_daytime\":%s,"
            "\"threshold_day\":%.1f,"
            "\"threshold_night\":%.1f,"
            "\"light_threshold\":%u}",
            *sys_state->temperature,
            *sys_state->light,
            *sys_state->relay_state ? "true" : "false",
            *sys_state->auto_mode ? "true" : "false",
            *sys_state->is_daytime ? "true" : "false",
            *sys_state->threshold_day,
            *sys_state->threshold_night,
            *sys_state->light_threshold
        );
        send_response(pcb, "200 OK", "application/json", json);
    } else if (strstr(request, "POST /relay")) {
        // Nu permitem comutarea releului daca este pe modul Auto
        if (*sys_state->auto_mode) {
            send_response(pcb, "200 OK", "application/json",
                "{\"success\":false,\"state\":false,\"mode\":\"auto\",\"error\":\"Nu se poate controla releul in modul automat\"}");
            return;
        }
        char *q = strstr(request, "state=");
        if (q) {
            bool new_state = (q[6] == '1');
            sys_state->set_relay(new_state);
            sys_state->update_display();
            
            char json[128];
            snprintf(json, sizeof(json),
                "{\"success\":true,\"state\":%s,\"mode\":\"manual\",\"error\":\"\"}",
                new_state ? "true" : "false");
            send_response(pcb, "200 OK", "application/json", json);
        } else {
            send_response(pcb, "400 Bad Request", "application/json", "{\"error\":\"Parametru lipsa\"}");
        }
    } else if (strstr(request, "POST /mode")) {
        char *q = strstr(request, "mode=");
        if (q) {
            bool new_auto = (strncmp(q + 5, "auto", 4) == 0);
            sys_state->set_mode(new_auto);
            sys_state->update_display();
            
            char json[128];
            snprintf(json, sizeof(json),
                "{\"success\":true,\"state\":%s,\"mode\":\"%s\",\"error\":\"\"}",
                *sys_state->relay_state ? "true" : "false",
                new_auto ? "auto" : "manual");
            send_response(pcb, "200 OK", "application/json", json);
        } else {
            send_response(pcb, "400 Bad Request", "application/json", "{\"error\":\"Parametru lipsa\"}");
        }
    } else if (strstr(request, "POST /threshold")) {
        char *d = strstr(request, "day=");
        char *n = strstr(request, "night=");
        char *l = strstr(request, "light=");
        
        if (d) *sys_state->threshold_day = strtof(d + 4, NULL);
        if (n) *sys_state->threshold_night = strtof(n + 6, NULL);
        if (l) *sys_state->light_threshold = (uint16_t)atoi(l + 6);
        
        sys_state->update_display();
        
        printf("[HTTP] Praguri actualizate: zi=%.1f, noapte=%.1f, lumina=%u\n",
               *sys_state->threshold_day, *sys_state->threshold_night,
               *sys_state->light_threshold);
        
        send_response(pcb, "200 OK", "application/json",
            "{\"success\":true,\"state\":false,\"mode\":\"\",\"error\":\"\"}");
    } else {
        send_response(pcb, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
    }
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        // Conexiune inchisa de client
        tcp_close(pcb);
        return ERR_OK;
    }
    
    // Preluam request-ul intr-un buffer local pentru siguranta (null-terminated)
    char request[256];
    int len = p->tot_len;
    if (len >= sizeof(request)) {
        len = sizeof(request) - 1;
    }
    
    pbuf_copy_partial(p, request, len, 0);
    request[len] = '\0';
    
    // Procesam request-ul HTTP
    handle_request(pcb, request);
    
    // Notificam lwIP ca am procesat datele si eliberam memoria
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    
    // Inchidem conexiunea dupa raspuns (comportament de API REST simplu)
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, http_recv);
    return ERR_OK;
}

bool http_server_init(http_system_state_t *state) {
    sys_state = state;
    
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        printf("[HTTP] EROARE: tcp_new a esuat\n");
        return false;
    }
    
    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("[HTTP] EROARE: tcp_bind a esuat\n");
        return false;
    }
    
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
    
    return true;
}
