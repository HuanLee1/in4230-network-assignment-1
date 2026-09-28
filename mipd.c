#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>

#include "common.h"

/*---------------------------------------*/
/* Prepare UNIX server socket for MIPD. */
/*-------------------------------------*/

static int prepare_server_sock(const char *socket_path){
    struct sockaddr_un addr;
    int sd = -1; 
    int rc = -1;

    /*1. Create socket:
    *       - AF_UNIX: Which local network we're using.
    *       - SOCK_SEQPACKET: How data is being sent.*/

    /* Error handling
    *  if sd is -1 that means socket failed.
    *  if sd is >= 0 that means the socket has a valid descriptor. */
    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(sd == -1){
        perror("socket");
        return -1;
    }
    
    /* Clearing &addr to avoid old bytes of data. */
    memset(&addr, 0, sizeof(addr));

    if(strlen(socket_path) >= sizeof(addr.sun_path)){
        fprintf(stderr, "Socket path too long\n");
        close(sd);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) -1);
    unlink(socket_path);


    /*2. Bind socket to filesystem path. */
    rc = bind(sd, (const struct sockaddr *)&addr, sizeof(addr));

    /* Check if bind was successful or not. */
    if(rc == -1){
        perror("bind");
        close(sd);
        return -1;
    }


    /*3. Mark socket as ready to accept client connections. */
    rc = listen(sd, MAX_CONNS);

    /* Check if listen() was successful or not. */
    if(rc == -1){
        perror("listen");
        close(sd);
        unlink(socket_path);
        return -1;
    }

    return sd;
}


/*-----------------------------------------------*/
/* Starts the MIPD and sets up the UNIX socket. */
/*---------------------------------------------*/

int main(int argc, char *argv[]){
    int server_sd;
    int raw_sock;
    int epoll_fd;
    int upper_sd = -1;
    int debug = 0;

    uint8_t local_mip;

    const char *socket_path;
    const char *mip_arg;

    struct mip_arp_cache arp_cache;
    struct epoll_event events[MAX_EVENTS];
    struct ifs_data ifs;
    struct pending_msg pending;    

    const unsigned short protocol = ETH_P_MIP;

    if(argc == 4 && strcmp(argv[1], "-d") == 0){
        debug = 1;
        socket_path = argv[2];
        mip_arg = argv[3];
    }
    else if(argc == 3){
        socket_path = argv[1];
        mip_arg = argv[2];
    }
    else {
        fprintf(stderr, "Usage: %s [-d] <socket_path> <mip_address>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr;
    long mip_value = strtol(mip_arg, &endptr, 10);

    if(endptr == mip_arg || *endptr != '\0' || mip_value < 0 || mip_value >= MIP_BROADCAST){
        fprintf(stderr, "Invalid MIP address. %s\n", mip_arg);
        return EXIT_FAILURE;
    }

    local_mip = (uint8_t)mip_value;




    mip_arp_cache_init(&arp_cache);
    memset(&pending, 0, sizeof(pending));

    server_sd = prepare_server_sock(socket_path);
    if(server_sd == -1){ 
        return EXIT_FAILURE;
    }
    
    raw_sock = socket(AF_PACKET, SOCK_RAW, htons(protocol));
    if(raw_sock == -1){
        perror("socket raw");
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    init_ifs(&ifs, raw_sock);

    if(debug){
        printf("mipd listening on UNIX socket: %s\n", socket_path);
        
        printf("Found %d network interfaces(s)\n", ifs.ifn);

        for(int i = 0; i<ifs.ifn; i++){
            printf("Interface %d - ifindex: %d - MAC: ", i, ifs.addr[i].sll_ifindex);
            print_mac_addr(ifs.addr[i].sll_addr);
            printf("\n");
        }
    }
    

  

    /* epoll_fd - Creates an epoll instance used to monitor multiple socket descriptors. 
    * Allows mipd to react to multiple client connections and network events without blocking on one socket at a time.
    * Examples: 
    *   - RAW socket descriptor
    *   - UNIX socket descriptor
    */
    epoll_fd = epoll_create1(0);
    if(epoll_fd == -1){
        perror("epoll_create1");
        close(raw_sock);
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    /* Creates a variable of type epoll_events.
    *   - Events: What kind of event we react to.
    *   - Data.fd: Whcih file descriptor the event belongs to.
    *
    * EPOLLIN means mipd reacts to when input is ready.
    * event.data.fd = server_sd means event is associated with server_sd. 
    * Server reacts when a new client wants to connect.
    */
    struct epoll_event unix_event;
    memset(&unix_event, 0, sizeof(unix_event));
    unix_event.events = EPOLLIN; 
    unix_event.data.fd = server_sd;

    /*Epoll_ctl controls what epoll is monitoring. 
*   - epoll_fd: the epoll instance / watchlist.
    *   - EPOLL_CTL_ADD: add the file descriptor we wanna monitor.
    *   - server_sd: the file descriptor we wanna monitor.
    *   - &events: How we wanna monitor: EPOLLIN & return server_sd as data.
    */
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sd, &unix_event) == -1){
        perror("epoll_ctl unix_sock");
        close(raw_sock);
        close(epoll_fd);
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }


    struct epoll_event raw_event;
    memset(&raw_event, 0, sizeof(raw_event));
    raw_event.events = EPOLLIN; 
    raw_event.data.fd = raw_sock;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, raw_sock, &raw_event) == -1){
        perror("epoll_ctl raw_sock");
        close(raw_sock);
        close(epoll_fd);
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }


    /* Wait continously for activity on any file descriptoer registered with epoll.  */
    while(1) {
        int num_events;
        
        /*Block untill one ore more registered file descriptor become ready. 
        * num_events tells us how many ervents epoll returned. 
        */
        num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if(num_events == -1){
            perror("epoll_wait");
            break;
        }

        /*Loop through every file descriptor that became ready during epoll_wait()*/
        for(int i = 0; i< num_events; i++){

            /*Retrive file descriptor associated with the event.*/
            int fd = events[i].data.fd;

            /* Activity on server_sd means a new UNIX local application is tryiung to connect to MIPD. */
            if(fd == server_sd){
                int new_sd = accept(server_sd, NULL, NULL);
                
                if(new_sd == -1){
                    perror("accept");
                    continue;
                }

                if(upper_sd != -1){
                    close(new_sd);
                    continue;
                }

                upper_sd = new_sd;


                /*Register newly connected application with epoll so MIPD is notified when the application sends data.*/
                struct epoll_event upper_event;
                memset(&upper_event, 0, sizeof(upper_event));
                upper_event.events = EPOLLIN;
                upper_event.data.fd = upper_sd;

                if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, upper_sd, &upper_event) == -1){
                    perror("epoll_ctl");
                    close(upper_sd);
                    upper_sd = -1;
                    continue;
                }
                
                if(debug){
                    printf("Upper-layer application connected\n");
                }
            } 
            /*Receive Ethernet frame from raw socket.*/
            else if(fd == raw_sock){
                struct sockaddr_ll so_name;
                struct ether_frame *frame_hdr;
                uint8_t buffer[1500];
                
                ssize_t bytes = recv_raw_packet(raw_sock, buffer, sizeof(buffer), &so_name);
                if(bytes == -1){
                    continue;
                }

                if(so_name.sll_pkttype == PACKET_OUTGOING){
                    continue;
                }

                if(bytes < (ssize_t)sizeof(struct ether_frame)){
                    continue;
                }

                frame_hdr = (struct ether_frame *)buffer;

                if(debug){
                    printf("Received Ethernet frame on ifindex %d\n", so_name.sll_ifindex);
                
                    printf("Source MAC: ");
                    print_mac_addr(frame_hdr->src_addr);
                    printf("\n");

                    printf("Destination MAC: ");
                    print_mac_addr(frame_hdr->dst_addr);
                    printf("\n");

                }

                uint8_t *mip_data;
                mip_data = get_payload_from_frame(buffer);
                size_t mip_len;
                mip_len = bytes - sizeof(struct ether_frame);

                struct mip_hdr mip_hdr;
                uint8_t *sdu;
                size_t sdu_len;
                
                if(mip_parse_pdu(mip_data, mip_len, &mip_hdr, &sdu, &sdu_len) == -1){
                    continue;
                }

                if(mip_hdr.dst != local_mip && mip_hdr.dst != MIP_BROADCAST){
                    continue;
                }

                if(mip_hdr.ttl == 0){
                    continue;
                }

                if(mip_hdr.sdu_type == MIP_TYPE_ARP){
                    handle_mip_arp(&arp_cache, &ifs, local_mip, &mip_hdr, sdu, sdu_len, frame_hdr->src_addr, so_name.sll_ifindex);
                    if(pending.valid && pending.msg.dst_mip == mip_hdr.src){
                        struct mip_arp_entry *entry;

                        entry = mip_arp_lookup(&arp_cache, pending.msg.dst_mip);
                        if(entry != NULL){
                            if(send_ping_via_entry(&ifs, raw_sock, local_mip, &pending.msg, entry) == -1){
                                fprintf(stderr,"Failed to send pending to MIP Ping\n");
                            }

                            pending.valid = 0;
                        }                     
                        
                    }
                    continue;
                }

                if(mip_hdr.sdu_type == MIP_TYPE_PING){
                    struct mip_app_msg app_msg;
                    app_msg.dst_mip = mip_hdr.src;

                    size_t copy_len = sdu_len;
                    if(copy_len > sizeof(app_msg.message) -1){
                        copy_len = sizeof(app_msg.message) -1;
                    }

                    memcpy(app_msg.message, sdu, copy_len);
                    app_msg.message[copy_len] = '\0';

                    if(upper_sd == -1){
                        continue;
                    }

                    if(send(upper_sd, &app_msg, sizeof(app_msg), 0) == -1){
                        perror("send to upper layer");
                    }
                }
            }
            else {
                struct mip_app_msg msg;
                ssize_t bytes = recv(fd, &msg, sizeof(msg), 0);
                if(bytes == -1){
                    perror("recv");
                    continue;
                }

                /*recv() returning 0 means that application closed it's connection. */
                if(bytes == 0){
                    /*remove the descriptor from epoll before closing it.*/
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);

                    if(fd == upper_sd){
                        upper_sd = -1;
                    }
                    continue;
                }

                if(bytes != (ssize_t)sizeof(msg)){
                    continue;
                }

                if(debug){
                    printf("Destination MIP: %u\n", msg.dst_mip);
                    printf("Message: %s\n", msg.message);
                }
          
                struct mip_arp_entry *entry;
                
                entry = mip_arp_lookup(&arp_cache, msg.dst_mip);
                if(entry == NULL){
                    pending.msg = msg;
                    pending.valid = 1;

                    if(send_mip_arp_request(&ifs, local_mip, msg.dst_mip) == -1){
                        fprintf(stderr, "Failed to send MIP-ARP request\n");
                        pending.valid = 0;
                    }

                    continue;
                }

                if(send_ping_via_entry(&ifs, raw_sock, local_mip, &msg, entry) == -1){
                    fprintf(stderr, "Failed to send MIP Ping \n");
                }
            }
        
        } 

    }

    if(upper_sd != -1){
        close(upper_sd);
    }
    close(raw_sock);
    close(epoll_fd);
    close(server_sd);
    unlink(socket_path);

    return EXIT_SUCCESS;
}