
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

#define MIP_TYPE_ARP  0x01
#define MIP_TYPE_PING 0x02
#define MIP_BROADCAST 0xFF

#define MIP_ARP_REQUEST  0
#define MIP_ARP_RESPONSE 1
#define MIP_ARP_CACHE_SIZE 256

#define ETH_BROADCAST {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}

#include <linux/if_packet.h>
#include <stdint.h>
#include <sys/types.h>


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
    uint8_t contents[];
}__attribute__((packed));

/*MIP header fields used internally before/after serialization.*/
struct mip_hdr {
    uint8_t dst;
    uint8_t src;
    uint8_t ttl;
    uint16_t sdu_len;
    uint8_t sdu_type;
};

struct mip_arp_msg{
    uint8_t type;
    uint8_t address;
};

struct mip_arp_entry {
    uint8_t mip_addr;
    uint8_t mac_addr[6];
    int ifindex;
    int valid;
};

struct mip_arp_cache {
    struct mip_arp_entry entries[MIP_ARP_CACHE_SIZE];
};

struct pending_msg{
    int valid;
    struct mip_app_msg msg;
};



/*Discover Discover available Ethernet interfaces and their link-layer addresses. */
void get_mac_from_interfaces(struct ifs_data *ifs);

/* Intiialize interface data and associate it with the raw socket.*/
void init_ifs(struct ifs_data *ifs, int raw_sock);

/*Print a MAC address in human-readable form.*/
void print_mac_addr(unsigned char *mac);

/*Builds and sends an Ethernet frame containing the supplied payload through selected interface.*/
int send_raw_packet(int sd, struct sockaddr_ll *so_name, const uint8_t *dst_addr, uint8_t *buf, size_t len);

/*Receives Ethernet frame and seperates its header from the payload so MIPD can inspect and process the packet further.*/
int recv_raw_packet(int sd, uint8_t *buf, size_t len, struct sockaddr_ll *src_addr);

/*Pack the MIP header fields into a 32-bit header. */
uint32_t mip_serialize_header(const struct mip_hdr *hdr);

/*Unpack a 32-bit MIP header into it's seperate fields.*/
void mip_parse_header(uint32_t raw_header, struct mip_hdr *hdr);

/*build a MIP PDU header into its seperate fields. */
ssize_t mip_build_pdu(struct mip_hdr *hdr, const uint8_t *sdu, size_t sdu_len, uint8_t *buf, size_t buf_len);

/*Round the SDU size up to a multiple of 4 bytes. */
size_t mip_padded_sdu_len(size_t len);

/*Convert padded SDU bytes into 32 bit words*/
uint16_t mip_sdu_words(size_t padded_len);

/*Parse a MIP PDU and retrive its header and sdu. */
int mip_parse_pdu(uint8_t *buf, size_t buf_len, struct mip_hdr *hdr, uint8_t **sdu, size_t *sdu_len);

void mip_arp_cache_init(struct mip_arp_cache *cache);

struct mip_arp_entry *mip_arp_lookup(struct mip_arp_cache *cache, uint8_t mip_addr);

void mip_arp_update(struct mip_arp_cache *cache, uint8_t mip_addr, const uint8_t *mac_addr, int ifindex);

uint32_t mip_arp_serialize(const struct mip_arp_msg *msg);

void mip_arp_parse(uint32_t raw_arp, struct mip_arp_msg *msg);

int send_mip_arp_request(struct ifs_data *ifs, uint8_t local_mip, uint8_t target_mip);


int send_mip_arp_response(struct ifs_data *ifs, int ifindex, const uint8_t *dst_mac, uint8_t local_mip, uint8_t requester_mip);

int handle_mip_arp(struct mip_arp_cache *cache, struct ifs_data *ifs, uint8_t local_mip, const struct mip_hdr *mip_hdr, const uint8_t *sdu, size_t sdu_len, const uint8_t *src_mac, int ifindex);

int send_ping_via_entry(struct ifs_data *ifs, int raw_sock, uint8_t local_mip, const struct mip_app_msg *msg, const struct mip_arp_entry *entry);

uint8_t *get_payload_from_frame(uint8_t *buf);

/*End of guard*/
#endif