#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include <stdbool.h>

// Initializeaza WiFi (conectare la AP) si porneste serverul HTTP pe portul 80.
// Returneaza true daca conexiunea WiFi si serverul s-au initializat cu succes.
bool wifi_server_init(void);

// Apeleaza periodic in bucla principala pentru procesarea pachetelor WiFi.
void wifi_server_poll(void);

// Returneaza adresa IP obtinuta de la DHCP (valid dupa wifi_server_init).
const char* wifi_server_get_ip(void);

#endif
