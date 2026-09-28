#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/ethernet.h>

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

/*--------------------------------------------------------------------------------------*/
/*Build the MIOP header taht will be placed before the SDU inside the Ethernet payload.*/
/*------------------------------------------------------------------------------------*/
size_t build_mip_header(uint8_t *buf, uint8_t src, uint8_t dst){

};
