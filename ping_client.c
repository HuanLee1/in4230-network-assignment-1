#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"

/*--------------------------------------------------------------------*/
/* Method that creates socket and connects it to server UNIX_socket. */
/*------------------------------------------------------------------*/
static int connect_to_mipd(const char *socket_path){
    struct sockaddr_un addr;
    int sd;

    /*Creates socket and checks if descriptor is valid. */
    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(sd == -1){
        perror("socket");
        return -1;
    }

    /*Initializes addr to avoid old data.*/
    memset(&addr, 0, sizeof(addr));

    /*Use UNIX domain socket, and set the path of the mipd socket.*/
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    /*Connect client socket to the MIPD UNIX socket. */
    if(connect(sd, (const struct sockaddr *)&addr, sizeof(addr)) == -1){
        perror("connect");
        close(sd);
        return -1;
    }

    return sd;
}


int main(int argc, char *argv[]){
    int sd;
    struct mip_app_msg msg;
    struct timespec start;
    struct timespec end;
    double rtt_ms;

    /*Input handling: 
    * Input has to have 4 argument counts before it's valid:
    *   - ./ping_client -> argc 1
    *   - socket_path -> argc 2
    *   - destination mip -> argc 3
    *   - message -> argc 4
    * Else it'll print out standard error and how to use ping_client.
    */
    if(argc < 4){
        fprintf(stderr, "Usage: %s <socket_path> <destination_mip> <message>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*Conmect the client to the local MIPD UNIX socket. */
    sd = connect_to_mipd(argv[1]);
    if(sd == -1){
        return EXIT_FAILURE;
    }
    
    /*Temporary test to verify if UNIX socket can communicate with MIPD. */
    /*Adding msg.dst_mip as argv[2]. Converting text string to integer using atoi. */
    msg.dst_mip = (unsigned char)atoi(argv[2]);

    /*Adding the message on argv[3] into msg. */
    snprintf(msg.message, sizof(msg.message), "PING:%s", argv[3]);

    clock_gettime(CLOCK_MONOTONIC, &start);

    /*send destination MIP - message to local MIPD over UNIX socket.*/
    if(send(sd, &msg, sizeof(msg), 0) == -1){
        perror("send");
        close(sd);
        return EXIT_FAILURE;
    }

    struct timeval timeout;

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    if (setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
        close(sd);
        return EXIT_FAILURE;
    }

    ssize_t bytes = recv(sd, &reply, sizeof(reply), 0);
    if (bytes == -1) {
        if(errno = EAGAIN || errno == EWOULDBLOCK){
            printf("timeout\n");
            close(sd);
            return EXIT_SUCCESS;
        }

        perror("recv");
        close(sd);
        return EXIT_FAILURE;
    }

    if (bytes == 0) {
        close(sd);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    rtt_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

    printf("Received from MIP %u: %s\n", reply.dst_mip, reply.message);

    printf("RTT: %.3f ms\n", rtt_ms);
    close(sd);

    return EXIT_SUCCESS;
}