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
            /*Save the interface info so we can use the MAC and ifindex when sending Ethernet frames.*/
            memcpy(&ifs->addr[i], (struct sockaddr_ll *)ifp->ifa_addr, sizeof(struct sockaddr_ll));
            i++;
        }
    }

    /*Store how many usable Ethernet interfaces were discovered.*/
    ifs->ifn = i;

    freeifaddrs(ifaces);
};

/*---------------------------------------------------------------------------------------*/
/* Initialize the interface structure and associate it with the raw socket used by MIPD.*/
/*-------------------------------------------------------------------------------------*/
void init_ifs(struct ifs_data *ifs, int raw_sock){
    get_mac_from_interfaces(ifs);
    ifs->rsock = raw_sock;
};

/*Prints MAC address in a six-byte hexidecimal notation*/
void print_mac_addr(unsigned char *mac){
    printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

};

/*Builds and sends a Ethernet frame with payload that's sent through selected network interface using raw socket.*/
int send_raw_packet(int sd, struct sockaddr_ll *so_name, uint8_t *dst_addr, uint8_t *buf, size_t len){
    struct ether_frame frame_hdr;
    struct msghdr *msg;
    struct iovec msgvec[2];
    int rc;

    /*Ethernet needs both destination MAC and the MAC address of the interface used to transmit the frame.*/
    memcpy(frame_hdr.dst_addr, dst_addr, 6);
    memcpy(frame_hdr.src_addr, so_name->sll_addr, 6);

    /*will be replaced later*/
    frame_hdr.eth_proto[0] = 0xFF;
    frame_hdr.eth_proto[1] = 0xFF;

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
};

/*-------------------------------------------------------------------------------------------*/
/*Receive Ethernet frame from raw socket and seperate the Ethernet header from the payload. */
/*-----------------------------------------------------------------------------------------*/
int recv_raw_packet(int sd, struct sockaddr_ll *so_name, struct ether_frame *frame_hdr, uint8_t *buf, size_t len){
    struct msghdr msg = {0};
    struct iovec msgvec[2];
    int rc;

    /* Split the frame into two memory regions:
    *   - msgvec[0]: Stores Ethernet header.
    *   - msgvec[1]: Stores the payload carried inside the frame.
    */
    msgvec[0].iov_base = frame_hdr;
    msgvec[0].iov_len = sizeof(struct ether_frame);
    msgvec[1].iov_base = buf;
    msgvec[1].iov_len = len;

    /*Tell recvmsg() where to store information about the received frame.
    *   - msg_name = so_name: link-layer destination.
    *   - msg_namelen: size of sockaddr_ll structure.
    *   - msg_iov: array containing ethernet header + payload.
    *   - msg_iovlen: Number of entries in the iovec array.
    */
    msg.msg_name = so_name;
    msg.msg_namelen = sizeof(struct sockaddr_ll);
    msg.msg_iov = msgvec;
    msg.msg_iovlen = 2;

    /*Receive one ethernet frame and distribute its contents into buffers described above.*/
    rc = recvmsg(sd, &msg, 0);

    if(rc == -1){
        perror("recvmsg");
        return -1;
    }

    /*Return number of bytes received.*/
    return rc;
};

/*---------------------------------------------------------------------*/
/* Put all the MIP header fields together into one 32-bit header format,
* so it can be sent over the network.
---------------------------------------------------------------------*/
uint32_t mip_seralize_header(const struct mip_hdr *hdr){
    uint32_t header = 0;

    header |= ((uint32_t)hdr->dst) << 24; 
    header |= ((uint32_t)hdr->src) << 16; 
    header |= ((uint32_t)(hdr->ttl & 0x0F)) << 12; 
    header |= ((uint32_t)(hdr->sdu_len & 0x01FF)) << 3; 
    header |= ((uint32_t)(hdr->sdu_type & 0x07));
    
    return htonl(header);
};

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
};

/*-----------------------------------------------------------------------*/
/*Builds a MIP PDU by seralizing the header and appending the padded SDU. 
* The finished PDU can be passed to the Ethernet layer for sending.
----------------------------------------------------------------------*/
size_t mip_build_pdu(struct mip_hdr *hdr, const uint8_t *sdu, size_t sdu_len, uint8_t *buf, size_t buf_len){
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
    raw_header = mip_seralize_header(hdr);
    
    /*Copy header into buffer.*/
    memcpy(buf, &raw_header, sizeof(raw_header));

    /*Copy sdu into buffer AFTER header.*/
    memcpy(buf + sizeof(raw_header), sdu, sdu_len);

    /*Fill with missing padding left*/
    memset(buf + sizeof(raw_header) + sdu_len, 0, padded_len - sdu_len);

    /*Return the size of the built MIP pdu in bytes. */
    return sizeof(raw_header) + padded_len;
};


/* Round the SDU length to the next multiple of 4 because MIP requires the payload to be 32-bit aligned. 
* Payload needs to fikt in 32-bits of blocks which is 4 bytes. 
* 
* Example: 
*   - "Hello" has 6 bytes. The MIP SDU requires it to be 32-aligned (dividable by 4),
*      so this function rounds it up to the closest divideable by 4 which would be 8.*/
size_t mip_padded_sdu_len(size_t len){
    return (len + 3) & ~((size_t)3); //Compact way of calculating the closest rounded up to 4
};


/*Convert padded SDU length from bytes to 32-bit words, 
* because MIP header stores the SDU length in units of 4 bytes.
* Example 8 padded bytes / 4 = 2 words. 
*/
uint16_t mip_sdu_words(size_t padded_len){
    return (uint16_t)(padded_len / 4);
};

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
};