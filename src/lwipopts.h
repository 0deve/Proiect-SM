#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H



// Mod bare-metal
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// ── Memorie ──
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (8 * 1024)

// Pool-uri de buffere
#define MEMP_NUM_TCP_PCB            5
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_PBUF               16
#define PBUF_POOL_SIZE              16

// ── Protocoale ──
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_ICMP                   1
#define LWIP_ARP                    1
#define LWIP_DNS                    0

// ── TCP tuning ──
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)

// ── Callback API (raw TCP) ──
#define LWIP_CALLBACK_API           1

// ── DHCP ──
#define DHCP_DOES_ARP_CHECK         0

// ── Debug (dezactivat) ──
#define LWIP_DEBUG                  0

#endif
