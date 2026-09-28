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


/*
* Create and prepares the UNIX domain server socket used by MIPD to communicate with the local upper-layer application.
*
* Socket_path: Path where the UNIX socket will be bound.
*
* Returns the listening socket descriptor on succdess, or -1 if socket creation, binding or listening fails.
*/
static int prepare_server_sock(const char *socket_path){
    struct sockaddr_un addr;
    int sd = -1; 
    int rc = -1;

    /* Create the local UNIX socket used by upper-layer applications to connect to mipd. */
    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(sd == -1){
        perror("socket");
        return -1;
    }
    
    /* Clear the address structure before filling in the UNIX socket fields. */
    memset(&addr, 0, sizeof(addr));

    /* Reject paths that do not fit in sockaddr_un.sun_path. */
    if(strlen(socket_path) >= sizeof(addr.sun_path)){
        fprintf(stderr, "Socket path too long\n");
        close(sd);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) -1);
    /*Unlink to remove any old socket file so bind() can reuse the same path. */
    unlink(socket_path); 


    /* Bind the UNIX socket to the filesystem path so applications can connect to it. */
    rc = bind(sd, (const struct sockaddr *)&addr, sizeof(addr));

    if(rc == -1){
        perror("bind");
        close(sd);
        return -1;
    }

    /* Start listening for upper-layer application connections. */
    rc = listen(sd, MAX_CONNS);

    if(rc == -1){
        perror("listen");
        close(sd);
        unlink(socket_path);
        return -1;
    }

    return sd;
}


/*
* Runs the MIP daemon.
*
* The daemon connects the local upper-layer application to the Ethernet network.
* It handles UNIX socket communication, raw Ethernet frames, MIP packet processing, MIP-ARP resolution, 
* ARP caching and forwarding ping payloads between application and the network.
*
* Returns EXIT_SUCCESS on normal termination and EXIT_FAILURE if initialization fails.
*
*/
int main(int argc, char *argv[]){
    /*File descriptors used by UNIX socket, raw socket and epoll.*/
    int server_sd;
    int raw_sock;
    int epoll_fd;
    int upper_sd = -1;

    /*Allows extra status output when MIPD is started with "-d".*/
    int debug = 0;

    /*MIP address assigned to this host.*/
    uint8_t local_mip;

    /* Command-line arguments for UNIX socket path and MIP address. */
    const char *socket_path;
    const char *mip_arg;

    /*^Runtime state used for ARP, interface handling and pending messages.*/
    struct mip_arp_cache arp_cache;
    struct epoll_event events[MAX_EVENTS];
    struct ifs_data ifs;
    struct pending_msg pending;    

    /*EtherType used to identify the MIP ethernet frames. */
    const unsigned short protocol = ETH_P_MIP;

    /*Read the the argv[] when the program started and find out if debug mode is on, 
    which UNIX socket path is gonan be used and which MIP-address this node will have.*/
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

    /*Convert the MIP address argument to a number and reject invalid values.
    * This prevents negative values and invalid inputs.
    */
    char *endptr;
    long mip_value = strtol(mip_arg, &endptr, 10);

    if(endptr == mip_arg || *endptr != '\0' || mip_value < 0 || mip_value >= MIP_BROADCAST){
        fprintf(stderr, "Invalid MIP address. %s\n", mip_arg);
        return EXIT_FAILURE;
    }

    local_mip = (uint8_t)mip_value;

    /* Start with an empty ARP cache and no pending message. */
    mip_arp_cache_init(&arp_cache);
    memset(&pending, 0, sizeof(pending));

    /* Prepare the UNIX socket used by local applications to communicate with mipd. */
    server_sd = prepare_server_sock(socket_path);
    if(server_sd == -1){ 
        return EXIT_FAILURE;
    }
    
    /* Create the raw Ethernet socket used to send and receive MIP frames. */
    raw_sock = socket(AF_PACKET, SOCK_RAW, htons(protocol));
    if(raw_sock == -1){
        perror("socket raw");
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    /* Discover usable Ethernet interfaces for raw frame transmission. */
    init_ifs(&ifs, raw_sock);

    /*Output text that appears when debug mode is on using "-d". */
    if(debug){
        printf("mipd listening on UNIX socket: %s\n", socket_path);
        
        printf("Found %d network interfaces(s)\n", ifs.ifn);

        for(int i = 0; i<ifs.ifn; i++){
            printf("Interface %d - ifindex: %d - MAC: ", i, ifs.addr[i].sll_ifindex);
            print_mac_addr(ifs.addr[i].sll_addr);
            printf("\n");
        }
    }
    

  

    /*Create an epoll isntance so MIPD can monitor several file descriptors at the same time. 
    * Examples: 
    *   - RAW socket descriptor.
    *   - UNIX socket descriptor.
    *
    * This lets MIPD react to whichever has input ready instead of blockign while waiting on only one socket.
    */
    epoll_fd = epoll_create1(0);
    if(epoll_fd == -1){
        perror("epoll_create1");
        close(raw_sock);
        close(server_sd);
        unlink(socket_path);
        return EXIT_FAILURE;
    }

    /* Describes what MIPD should watch for on the UNIX server socket. 
    * EPOLLIN means we're interested when input is ready.
    * for server_sd this means a new upper-layer application trying to connect.
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
        
        /*Block untill one ore mroe registered file descriptor becomes ready.
        * num_events tells us how many events epoll_wait() returned.
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

                /*
                * Only one upper-layer application is supported at a time.
                * Reject any additional connection while one is already active. 
                */
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

                /*Add upper_sd to the epoll watchlist.*/
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

            /*Handle incoming Ethernet traffic from raw socket.
            * Received frame is stored in buffer together with metadata about which itnerface it arrived on.
            */
            else if(fd == raw_sock){
                struct sockaddr_ll so_name;
                struct ether_frame *frame_hdr;
                uint8_t buffer[1500];
                
                /*Receive one raw Ethernet frame into buffer. */
                ssize_t bytes = recv_raw_packet(raw_sock, buffer, sizeof(buffer), &so_name);
                if(bytes == -1){
                    continue;
                }

                /*With raw sockets, you are able to see your own outgoing ethernet frames. 
                * We're ignoring outgoing packets to avoid reacting to our own packets. 
                */
                if(so_name.sll_pkttype == PACKET_OUTGOING){
                    continue;
                }

                /*Ethernet frame bust be large enough to contain the header, so we 
                ignore shorter frames to avoid reading incompelte data as valid header .*/
                if(bytes < (ssize_t)sizeof(struct ether_frame)){
                    continue;
                }

                 /*Treat the beginning of the received buffer as an Ethernet frame header,
                so we can access fields such as source and destination MAC addresses.*/
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

               /*Access the MIP packet from ethernet payload  and calculate the packet length
               * so that we can later parse it and know how much valid MIP data is available in the Ethernet payload.*/
                uint8_t *mip_data;
                mip_data = get_payload_from_frame(buffer);
                size_t mip_len;
                mip_len = bytes - sizeof(struct ether_frame);


                /*Parse the MIP packet into its header and SDU.
                Invalid or malformed packets are ignored. */
                struct mip_hdr mip_hdr;
                uint8_t *sdu;
                size_t sdu_len;
                
                if(mip_parse_pdu(mip_data, mip_len, &mip_hdr, &sdu, &sdu_len) == -1){
                    continue;
                }

                /*Only process packets addfressed to this host or sent as MIP broadcast.*/
                if(mip_hdr.dst != local_mip && mip_hdr.dst != MIP_BROADCAST){
                    continue;
                }

                /*Ignore packets whose TTL has expired.*/
                if(mip_hdr.ttl == 0){
                    continue;
                }

                /*Handle incoming MIP-ARP packets. 
                * The ARP handler updates the cache and may send a reply.
                * If we were waiting for this MIP address to be resolved, try the cache again and send the pending ping. 
                */
                if(mip_hdr.sdu_type == MIP_TYPE_ARP){
                    /*Process the ARP packet and update the cache if needed.*/
                    handle_mip_arp(&arp_cache, &ifs, local_mip, &mip_hdr, sdu, sdu_len, frame_hdr->src_addr, so_name.sll_ifindex);
                    if(pending.valid && pending.msg.dst_mip == mip_hdr.src){
                        struct mip_arp_entry *entry;

                        /*If we waited for this, send it to MIP-PING*/
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

                /*Handle incoming MIP ping packets.
                *Copy the ping SDU into an application message and deliver it
                * to the connected upper-layer application through the unix socket. 
                */
                if(mip_hdr.sdu_type == MIP_TYPE_PING){
                    struct mip_app_msg app_msg;
                    app_msg.dst_mip = mip_hdr.src;


                    /* app_msg.message has a fixed size, so we limit how much of the SDU we copy.
                    * This avoids writing outside the buffer if the MIP Ping message is too large.
                    * One byte is kept free for '\0'.*/
                    size_t copy_len = sdu_len;
                    if(copy_len > sizeof(app_msg.message) -1){
                        copy_len = sizeof(app_msg.message) -1;
                    }

                    /* Copy the SDU into app_msg and end it properly as a C string. */
                    memcpy(app_msg.message, sdu, copy_len);
                    app_msg.message[copy_len] = '\0';

                    /* Drop the packet if no upper-layer application is connected. */
                    if(upper_sd == -1){
                        continue;
                    }

                    /* Send the completed application message to the connected upper-layer process. */
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

                /* Ignore incomplete application messages. */
                if(bytes != (ssize_t)sizeof(msg)){
                    continue;
                }

                if(debug){
                    printf("Destination MIP: %u\n", msg.dst_mip);
                    printf("Message: %s\n", msg.message);
                }
          
                /*
                * Check whether we already know the MAC address for the destination MIP.
                */
                struct mip_arp_entry *entry;
                entry = mip_arp_lookup(&arp_cache, msg.dst_mip);

                /*
                * If the destination is not in the ARP cache, save the ping message and 
                * send a MIP-ARP reques tfirst. The ping will be sent later when the destination MAC address has been learned.
                */
                if(entry == NULL){
                    pending.msg = msg;
                    pending.valid = 1;

                    if(send_mip_arp_request(&ifs, local_mip, msg.dst_mip) == -1){
                        fprintf(stderr, "Failed to send MIP-ARP request\n");
                        pending.valid = 0;
                    }

                    continue;
                }

                /*
                * The destination is already known in the ARP cache,
                * so the Ping can be sent immediately.
                */
                if(send_ping_via_entry(&ifs, raw_sock, local_mip, &msg, entry) == -1){
                    fprintf(stderr, "Failed to send MIP Ping \n");
                }
            }
        
        } 

    }

    /*Close all open sockets and remove the UNIX socket file before exiting.*/
    if(upper_sd != -1){
        close(upper_sd);
    }
    close(raw_sock);
    close(epoll_fd);
    close(server_sd);
    unlink(socket_path);

    return EXIT_SUCCESS;
}