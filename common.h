
/* Guard helps prevent common.h from being included multiple times.
* Else it'll duplicate stuff during compilation.
*/
#ifndef _COMMON_H
#define _COMMON_H

/* Limits used by the UNIX socket / epoll handling,
* and inteface discovery.
*/
#define MAX_EVENTS 10
#define MAX_CONNS 5
#define MAX_IFACES 10
#define ETH_P_MIP 0x88B5
#define MIP_HDR_LEN 4

#include <linux/if_packet.h>
#include <stdint.h>


/* Message format used between local application and the MIPD over the UNIX socket. */
struct mip_app_msg {
    unsigned char dst_mip;
    char message[256];
};

/* Stores ethernet itnerface information discovered by MIPD. 
* addr[] contains sockaddr_ll enmtries with MAC address and ifindex.
*/
struct ifs_data{
    struct sockaddr_ll addr[MAX_IFACES];
    int ifn;
    int rsock;
};

/*Represents Ethernet header placed in front of a payload. MIPD must build this header because it sends frames through a raw socket.*/
struct ether_frame{
    uint8_t dst_addr[6];
    uint8_t src_addr[6];
    uint8_t eth_proto[2];
    uint8_t contents[0];
}__attribute__((packed));

struct mip_packet {
    uint8_t src;
    uint8_t dst;
    uint8_t type;
    uint8_t ttl;

    uint8_t *sdu;
    size_t sdu_len;
}

/*Discover Discover available Ethernet interfaces and their link-layer addresses. */
void get_mac_from_interfaces(struct ifs_data *ifs);

/* Intiialize itnerface data and associate it with the raw socket.*/
void init_ifs(struct ifs_data *ifs, int raw_sock);

/*Print a MAC address in human-readable form.*/
void print_mac_addr(unsigned char *mac);

/*Builds and sends an Ethernet frame containing the supplied payload through selected interface.*/
int send_raw_packet(int sd, struct sockaddr_ll *so_name, uint8_t *dst_addr, uint8_t *buf, size_t len);

/*Receives Ethernet frame and seperates its header from the payload so MIPD can inspect and process the packet further.*/
int recv_raw_packet(int sd, struct sockaddr_ll *so_name, struct ether_frame *frame_hdr, uint8_t *buf, size_t len);

size_t build_mip_header(uint8_t *buf, uint8_t src, uint8_t dst);

/*End of guard*/
#endif