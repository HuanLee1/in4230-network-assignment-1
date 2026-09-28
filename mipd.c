#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
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

    /*Set socket family to UNIX domain,
    * and copying socket_path int othe address structure.
    * Unlinking to remove old "leftover" data from socket/file. */
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
    }

    return sd;
};


/*-----------------------------------------------*/
/* Starts the MIPD and sets up the UNIX socket. */
/*---------------------------------------------*/

int main(int argc, char *argv[]){
    int server_sd;
    int raw_sock;
    int client_sd;
    int epoll_fd;

    struct mip_app_msg msg;
    struct epoll_event events[MAX_EVENTS];
    struct ifs_data ifs;
    
    unsigned short protocol = 0xFFFF;

    /*Input check before MIPD start. 
    * Example:
    * ./mipd -> argc = 1. (invalid prints out standard error anx exists)
    * ./mipd /tmp/mip_socket -> argc = 2. (valid, continue) */
    if(argc < 2){
        fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*Creates UNIX server socket using path provided & error handling.*/
    server_sd = prepare_server_sock(argv[1]);
    if(server_sd == -1){ 
        return EXIT_FAILURE;
    }
    
    /*Create a raw AF_PACKET socket so mipd can send and receive ethernet frames directly on the local network interface.*/
    raw_sock = socket(AF_PACKET, SOCK_RAW, htons(protocol));
    if(raw_sock == -1){
        perror("socket raw");
        close(server_sd);
        unlink(argv[1]);
        return EXIT_FAILURE;
    }

      /* Server is now listening for a local application connection. */
    printf("mipd listening on UNIX socket: %s\n", argv[1]);
    

    /*Discxover ethernet interfaces and store information such as MAC addresses and interfaces.*/
    init_ifs(&ifs, raw_sock);

    /*Print discovered interfaces to verify that interfaces setup succeeded.*/
    printf("Found %d network interfaces(s)\n", ifs.ifn);

    for(int i = 0; i<ifs.ifn; i++){
        printf("Interface %d - ifindex: %d - MAC: ", i, ifs.addr[i].sll_ifindex);
        print_mac_addr(ifs.addr[i].sll_addr);
        printf("\n");
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
        close(server_sd);
        unlink(argv[1]);
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
        close(epoll_fd);
        close(server_sd);
        unlink(argv[1]);
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
        unlink(argv[1]);
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
                client_sd = accept(server_sd, NULL, NULL);
                if(client_sd == -1){
                    perror("accept");
                    continue;
                }

                /*Register newly connected application with epoll so MIPD is notified when the application sends data.*/
                struct epoll_event client_event;
                memset(&client_event, 0, sizeof(client_event));
                client_event.events = EPOLLIN;
                client_event.data.fd = client_sd;

                if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sd, &client_event) == -1){
                    perror("epoll-ctl");
                    close(client_sd);
                    continue;
                }
                
                printf("Client connected to mipd\n");
            } 
            /*Receive Ethernet frame from raw socket.*/
            else if(fd == raw_sock){
                struct sockaddr_ll so_name;
                struct ether_frame frame_hdr;
                uint8_t buffer[1500];
                
                int bytes = recv_raw_packet(raw_sock, &so_name, &frame_hdr, buffer, sizeof(buffer));
                if(bytes == -1){
                    continue;
                }

                printf("Received Ethernet frame on ifindex %d\n", so_name.sll_ifindex);
                printf("Source MAC: ");
                print_mac_addr(frame_hdr.src_addr);
                printf("\n");

                printf("Destination MAC: ");
                print_mac_addr(frame_hdr.dst_addr);
                printf("\n");
            }
            else {
                /*Activity on another registered descriptor means an already connected application has sent data to MIPD. */
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
                    continue;
                }

                printf("Destinatiom MIP: %u\n", msg.dst_mip);
                printf("Message: %s\n", msg.message);

                uint8_t broadcast_mac[6] = { 0xff,  0xff,  0xff,  0xff,  0xff,  0xff };
                if(ifs.ifn > 0){
                    send_raw_packet(raw_sock, &ifs.addr[0], broadcast_mac, (uint8_t *)msg.message, strlen(msg.message) +1);
                }
            }
        
        
        } 

    }

    close(client_sd);
    close(server_sd);
    unlink(argv[1]);

    return EXIT_SUCCESS;
};