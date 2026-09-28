#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include "common.h"

/*-----------------------------------------------------------------------------------------------------*/
/*Get all usable network interfaces so MIPD knows which MAC addresses and interfaces it can send from.*/
/*---------------------------------------------------------------------------------------------------*/

void get_mac_from_interfaces(struct ifs_data *ifs){
    struct ifaddrs *ifaces;
    struct ifaddrs *ifp;
    int i = 0;

    /*Retrive the list of interfaces available on the host.*/
    if(getifaddrs(&ifaces) == -1){
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    /*Keep only ethernet-capable interfaces and ignore loopback.*/
    for(ifp = ifaces; ifp != NULL; ifp = ifp->ifa_next){

        /*Filtering we're using. 
        ignoring NULL and loopback. 
        */
        if(ifp->ifa_addr != NULL &&
        ifp->ifa_addr->sa_family == AF_PACKET &&
        strcmp("lo", ifp->ifa_name) != 0){
            if(i >= MAX_IFACES){
                break;
            }

            /*Save the interface info so we can use the MAC and ifindex when sending Ethernet frames.*/
            memcpy(&ifs->addr[i], (struct sockaddr_ll *)ifp->ifa_addr, sizeof(struct sockaddr_ll));
            i++;
        }
    }

    /*Store how many usable Ethernet interfaces were discovered.*/
    ifs->ifn = i;

    freeifaddrs(ifaces);
}

/*---------------------------------------------------------------------------------------*/
/* Initialize the interface structure and associate it with the raw socket used by MIPD.*/
/*-------------------------------------------------------------------------------------*/
void init_ifs(struct ifs_data *ifs, int raw_sock){
    get_mac_from_interfaces(ifs);
    ifs->rsock = raw_sock;
}

/*Prints MAC address in a six-byte hexidecimal notation*/
void print_mac_addr(unsigned char *mac){
    printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

}

/*Builds and sends a Ethernet frame with payload that's sent through selected network interface using raw socket.*/
int send_raw_packet(int sd, struct sockaddr_ll *so_name, const uint8_t *dst_addr, uint8_t *buf, size_t len){
    struct ether_frame frame_hdr;
    struct msghdr *msg;
    struct iovec msgvec[2];
    int rc;

    /*Ethernet needs both destination MAC and the MAC address of the interface used to transmit the frame.*/
    memcpy(frame_hdr.dst_addr, dst_addr, 6);
    memcpy(frame_hdr.src_addr, so_name->sll_addr, 6);

    /*will be replaced later*/
    uint16_t ethertype = htons(ETH_P_MIP);
    memcpy(frame_hdr.eth_proto, &ethertype, sizeof(ethertype));

    /* Split the frame into two memory regions:
    *   - msgvec[0]: Stores Ethernet header.
    *   - msgvec[1]: Stores the payload carried inside the frame.
    */
    msgvec[0].iov_base = &frame_hdr;
    msgvec[0].iov_len = sizeof(struct ether_frame);
    msgvec[1].iov_base = buf;
    msgvec[1].iov_len = len;

    /*Allocate and clean msghdr so unused/old metadata starts as zero. */
    msg = calloc(1, sizeof(struct msghdr));

    if(msg == NULL){
        perror("calloc");
        return -1;
    }
    
    /*Tell sendmsg() where and what to send.
    *   - msg_name = so_name: link-layer destination.
    *   - msg_namelen: size of sockaddr_ll structure.
    *   - msg_iov: array containing ethernet header + payload.
    *   - msg_iovlen: Number of entries in the iovec array.
    */
    msg->msg_name = so_name;
    msg->msg_namelen = sizeof(struct sockaddr_ll);
    msg->msg_iov = msgvec;
    msg->msg_iovlen = 2;

    /*Send message */
    rc = sendmsg(sd, msg, 0);

    if(rc == -1){
        perror("sendmsg");
        free(msg);
        return -1;
    }

    free(msg);
    return rc;
}

/*-------------------------------------------------------------------------------------------*/
/*Receive Ethernet frame from raw socket and seperate the Ethernet header from the payload. */
/*-----------------------------------------------------------------------------------------*/
int recv_raw_packet(int sd, uint8_t *buf, size_t len, struct sockaddr_ll *src_addr){
    struct msghdr msg;
    struct iovec iov;
    ssize_t rc;

    memset(&msg, 0, sizeof(msg));
    memset(src_addr, 0, sizeof(*src_addr));

    iov.iov_base = buf;
    iov.iov_len = len;
    
    msg.msg_name = src_addr;
    msg.msg_namelen = sizeof(*src_addr);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    rc = recvmsg(sd, &msg, 0);

    if(rc == -1){
        perror("recvmsg");
        return -1;
    }

    return rc;
}

/*---------------------------------------------------------------------*/
/* Put all the MIP header fields together into one 32-bit header format,
* so it can be sent over the network.
---------------------------------------------------------------------*/
uint32_t mip_serialize_header(const struct mip_hdr *hdr){
    uint32_t header = 0;

    header |= ((uint32_t)hdr->dst) << 24; 
    header |= ((uint32_t)hdr->src) << 16; 
    header |= ((uint32_t)(hdr->ttl & 0x0F)) << 12; 
    header |= ((uint32_t)(hdr->sdu_len & 0x01FF)) << 3; 
    header |= ((uint32_t)(hdr->sdu_type & 0x07));
    
    return htonl(header);
}

/*------------------------------------------------------*/
/* Retrive the header fields from a 32-bit MIP header. */
/*----------------------------------------------------*/
void mip_parse_header(uint32_t raw_header, struct mip_hdr *hdr){
    uint32_t header = ntohl(raw_header);
    hdr->dst = (header >> 24) & 0xFF;
    hdr->src = (header >> 16) & 0xFF;
    hdr->ttl = (header >> 12) & 0x0F;
    hdr->sdu_len= (header >> 3) & 0x01FF;
    hdr->sdu_type = header & 0x07;
}

/*-----------------------------------------------------------------------*/
/*Builds a MIP PDU by serializing the header and appending the padded SDU. 
* The finished PDU can be passed to the Ethernet layer for sending.
----------------------------------------------------------------------*/
ssize_t mip_build_pdu(struct mip_hdr *hdr, const uint8_t *sdu, size_t sdu_len, uint8_t *buf, size_t buf_len){
    uint32_t raw_header;
    size_t padded_len;

    /*Pad the SDU so it's size follows MIP's 32-bit requirement. */
    padded_len = mip_padded_sdu_len(sdu_len);

    /*Check that the buffer ahs enough space for the whole PDU. 
    *   - if no -> stop
    *   - if yes -> build PDU
    */
    if(buf_len < sizeof(uint32_t) + padded_len){
        return -1;
    }

    /*Store the padded SDU length as 32-bit words in the MIP header*/
    hdr->sdu_len = mip_sdu_words(padded_len);

    /*Pack the MIP header fields into the 32-bit formated used by on the network.*/
    raw_header = mip_serialize_header(hdr);
    
    /*Copy header into buffer.*/
    memcpy(buf, &raw_header, sizeof(raw_header));

    /*Copy sdu into buffer AFTER header.*/
    memcpy(buf + sizeof(raw_header), sdu, sdu_len);

    /*Fill with missing padding left*/
    memset(buf + sizeof(raw_header) + sdu_len, 0, padded_len - sdu_len);

    /*Return the size of the built MIP pdu in bytes. */
    return sizeof(raw_header) + padded_len;
}


/* Round the SDU length to the next multiple of 4 because MIP requires the payload to be 32-bit aligned. 
* Payload needs to fikt in 32-bits of blocks which is 4 bytes. 
* 
* Example: 
*   - "Hello" has 6 bytes. The MIP SDU requires it to be 32-aligned (dividable by 4),
*      so this function rounds it up to the closest divideable by 4 which would be 8.*/
size_t mip_padded_sdu_len(size_t len){
    return (len + 3) & ~((size_t)3); //Compact way of calculating the closest rounded up to 4
}


/*Convert padded SDU length from bytes to 32-bit words, 
* because MIP header stores the SDU length in units of 4 bytes.
* Example 8 padded bytes / 4 = 2 words. 
*/
uint16_t mip_sdu_words(size_t padded_len){
    return (uint16_t)(padded_len / 4);
}

/*------------------------------------------------------------------------------------------------*/
/*Parse a received MIP PDU by extracting the MIP header and finding the SDU that comes after it. */
/*----------------------------------------------------------------------------------------------*/
int mip_parse_pdu(uint8_t *buf, size_t buf_len, struct mip_hdr *hdr, uint8_t **sdu, size_t *sdu_len){
    uint32_t raw_header;
    size_t payload_len;

    /*A valid MIP PDU must at least coantain the 4-byte MIP header. */
    if(buf_len < sizeof(uint32_t)){
        return -1;
    }

    /* Copy the first 4 bytes from the buffer which contain the MIP header. */
    memcpy(&raw_header, buf, sizeof(raw_header));

    /*Unpack the 32-bit header into seperate MIP header fields.*/
    mip_parse_header(raw_header, hdr);

    /*MIP stores SDU length in 32-bit words, so convert it back to bytes.*/
    payload_len = hdr->sdu_len * 4;

    /*Make sure that the received buffer actually contains the complete SDU:*/
    if(buf_len < sizeof(uint32_t) + payload_len){
        return -1;
    }

    /*Point to the SDU which starts directly after the 4-byte MIP header.*/
    *sdu = buf + sizeof(uint32_t);

    /*Store how many bytes the SDU contains.*/
    *sdu_len = payload_len;

    return 0;
}


void mip_arp_cache_init(struct mip_arp_cache *cache){
    memset(cache, 0, sizeof(*cache));
}

struct mip_arp_entry *mip_arp_lookup(struct mip_arp_cache *cache, uint8_t mip_addr){
    struct mip_arp_entry *entry;

    entry = &cache->entries[mip_addr];

    if(!entry->valid){
        return NULL;
    }

    return entry;
}

void mip_arp_update(struct mip_arp_cache *cache, uint8_t mip_addr, const uint8_t *mac_addr, int ifindex){
    struct mip_arp_entry * entry;

    entry = &cache->entries[mip_addr];

    entry->mip_addr = mip_addr;
    memcpy(entry->mac_addr, mac_addr, 6);
    entry->ifindex = ifindex;
    entry->valid = 1;
}


uint32_t mip_arp_serialize(const struct mip_arp_msg *msg){
    uint32_t arp = 0;

    arp |= ((uint32_t)(msg->type & 0x01)) << 31;
    arp |= ((uint32_t)msg->address) << 23;

    return htonl(arp);
}


void mip_arp_parse(uint32_t raw_arp, struct mip_arp_msg *msg){
    uint32_t arp = ntohl(raw_arp);

    msg->type = (arp >> 31) & 0x01;
    msg->address = (arp >> 23) & 0xFF;
}

int send_mip_arp_request(struct ifs_data *ifs, uint8_t local_mip, uint8_t target_mip){
    struct mip_arp_msg arp_msg;
    struct mip_hdr mip_hdr;

    uint32_t arp_sdu;
    uint8_t mip_pdu[512];

    uint8_t broadcast_mac[] = ETH_BROADCAST;

    ssize_t mip_len;
    
    arp_msg.type = MIP_ARP_REQUEST;
    arp_msg.address = target_mip;

    arp_sdu = mip_arp_serialize(&arp_msg);

    mip_hdr.dst = MIP_BROADCAST;
    mip_hdr.src = local_mip;
    mip_hdr.ttl = 1;
    mip_hdr.sdu_len = 0;
    mip_hdr.sdu_type = MIP_TYPE_ARP;

    mip_len = mip_build_pdu(&mip_hdr, (uint8_t *)&arp_sdu, sizeof(arp_sdu), mip_pdu, sizeof(mip_pdu));

    if(mip_len == -1){
        return -1;
    }

    for(int i = 0; i < ifs->ifn; i++){
        if(send_raw_packet(ifs->rsock, &ifs->addr[i], broadcast_mac, mip_pdu, mip_len) == -1){
            perror("send_raw_packet");
            return -1;
        }
    }

    return 0;
}

 

int send_mip_arp_response(struct ifs_data *ifs, int ifindex, const uint8_t *dst_mac, uint8_t local_mip, uint8_t requester_mip){
    struct mip_arp_msg arp_msg;
    struct mip_hdr mip_hdr;

    uint32_t arp_sdu;
    uint8_t mip_pdu[512];

    ssize_t mip_len;

    arp_msg.type = MIP_ARP_RESPONSE;
    arp_msg.address = local_mip;

    arp_sdu = mip_arp_serialize(&arp_msg);

    mip_hdr.dst = requester_mip;
    mip_hdr.src = local_mip;
    mip_hdr.ttl = 1;
    mip_hdr.sdu_len = 0;
    mip_hdr.sdu_type = MIP_TYPE_ARP;

    mip_len = mip_build_pdu(&mip_hdr, (uint8_t *)&arp_sdu, sizeof(arp_sdu), mip_pdu, sizeof(mip_pdu));
    if(mip_len == -1){
        return -1;
    }

    for(int i = 0; i < ifs->ifn; i++){

        if(ifs->addr[i].sll_ifindex == ifindex){
            return send_raw_packet(ifs->rsock, &ifs->addr[i], dst_mac, mip_pdu, mip_len);
        }
    }

    return -1;
}

int handle_mip_arp(struct mip_arp_cache *cache, struct ifs_data *ifs, uint8_t local_mip, const struct mip_hdr *mip_hdr, const uint8_t *sdu, size_t sdu_len, const uint8_t *src_mac, int ifindex){
    struct mip_arp_msg arp_msg;
    uint32_t raw_arp;

    if(mip_hdr->sdu_type != MIP_TYPE_ARP){
        return 0;
    }

    if(sdu_len < sizeof(uint32_t)){
        return -1;
    }

    memcpy(&raw_arp, sdu, sizeof(raw_arp));
    mip_arp_parse(raw_arp, &arp_msg);

    mip_arp_update(cache, mip_hdr->src, src_mac, ifindex);

    if(arp_msg.type == MIP_ARP_REQUEST){
        if(arp_msg.address == local_mip){
            return send_mip_arp_response(ifs, ifindex, src_mac, local_mip, mip_hdr->src);
        }

        return 0;
    }

    if(arp_msg.type == MIP_ARP_RESPONSE){
        return 0;
    }
    return -1;

}


int send_ping_via_entry(
    struct ifs_data *ifs, int raw_sock, uint8_t local_mip, const struct mip_app_msg *msg, const struct mip_arp_entry *entry){
    struct mip_hdr mip_hdr;
    uint8_t mip_pdu[1500];
    ssize_t mip_len;

    mip_hdr.dst = msg->dst_mip;
    mip_hdr.src = local_mip;
    mip_hdr.ttl = 15;
    mip_hdr.sdu_len = 0;
    mip_hdr.sdu_type = MIP_TYPE_PING;

    mip_len = mip_build_pdu(&mip_hdr, (const uint8_t *)msg->message, strlen(msg->message) + 1, mip_pdu, sizeof(mip_pdu));

    if (mip_len == -1) {
        return -1;
    }

    for (int i = 0; i < ifs->ifn; i++) {

        if (ifs->addr[i].sll_ifindex == entry->ifindex) {

            return send_raw_packet(raw_sock, &ifs->addr[i], entry->mac_addr, mip_pdu, mip_len);
        }
    }

    return -1;
}

uint8_t *get_payload_from_frame(uint8_t *buf){
    struct ether_frame *frame;
    frame = (struct ether_frame *)buf;
    return frame->contents;
}

