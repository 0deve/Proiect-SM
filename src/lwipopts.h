// lwipopts.h - configurare lwip pentru serverul web al pico w
// aceste valori sunt necesare pt compilare si controleaza comportarea stivei tcp/ip

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// modul fara sistem de operare (bare-metal)
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// activam modulele necesare
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_ICMP                   1
#define LWIP_DNS                    0

// dimensiuni buffere
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_PCB            5
#define MEMP_NUM_TCP_SEG            16
#define PBUF_POOL_SIZE              16
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_WND                     (4 * TCP_MSS)

// callbacks
#define LWIP_CALLBACK_API           1
#define LWIP_HTTPD                  0

// statistici si debug (dezactivate pt release)
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
