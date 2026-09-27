#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>

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
    int client_sd;
    int epoll_fd;
    struct mip_app_msg msg;

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

    /* Server is now listening for a local application connection. */
    printf("mipd listening on UNIX socket: %s\n", argv[1]);
    

    /*Epoll setup.*/
    epoll_fd = epollcreate1(0);
    if(epoll_fd == -1){
        perror("epollcreate1");
        close(server_sd);
        unlink(argv[1]);
        return EXIT_FAILURE;
    }

    struct epoll_event event;
    memset(&event, 0, sizeof(event));

    event.events = EPOLLIN;
    event.data.fd = server_sd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sd, &event) == -1){
        perror("epoll_ctl");
        close(epoll_fd);
        close(server_sd);
        unlink(argv[1]);
        return EXIT_FAILURE;
    }

    epoll_wait();

    /* Wait for and accept one client connection (temporary). */
    client_sd = accept(server_sd, NULL, NULL);
    if(client_sd == -1){
        perror("accept");
        close(server_sd);
        unlink(argv[1]);
        return EXIT_FAILURE;
    }

    printf("Client connected to MIPD\n");

    /*Temporary test to verify that MIPD can receive data from a connected UNIX SOCKET client.*/
    ssize_t bytes = recv(client_sd, &msg, sizeof(msg), 0);
    if(bytes == -1){
        perror("recv");
        close(client_sd);
        close(server_sd);
        unlink(argv[1]);
        return EXIT_FAILURE;
    }

    printf("Destination MIP: %u\n", msg.dst_mip);
    printf("Message: %s\n", msg.message);

    close(client_sd);
    close(server_sd);
    unlink(argv[1]);

    return EXIT_SUCCESS;
};