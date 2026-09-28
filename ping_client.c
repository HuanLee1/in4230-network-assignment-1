#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"

/*
* Connect to the local MIPd through a UNIX domain socket.
*
* Socket_path: path to the UNIX socket exposed by MIPD.

* Returns the connected socket descriptor on success.
* Returns -1 if socket creation or connect() fails. 
*/
static int connect_to_mipd(const char *socket_path){
    struct sockaddr_un addr;
    int sd;

    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(sd == -1){
        perror("socket");
        return -1;
    }

    /*Clear address structure before filling in the UNIX socket fields to avoid old data.*/
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if(connect(sd, (const struct sockaddr *)&addr, sizeof(addr)) == -1){
        perror("connect");
        close(sd);
        return -1;
    }

    return sd;
}

/*
* Run the ping_client.
*
* argc: the total number of command-line arguments.
* argv: Command-line arguments:
*   - argv[1]: Path to local MIPD UNIX socket.
*   - argv[2]: User-specified message.
*   - argv[3]: Destination MIP address.
* 
* Builds a "PING:<message>" request and sends it to MIPD.
* Waits up to one second for a reply and prints the RTT.
* 
* Returns EXIT_SUCCESS after a reply or timeout.
* Returns EXIT_FAILURE on invalid arguments or socket errors.
*/
int main(int argc, char *argv[]){
    int sd; 
    struct mip_app_msg msg; 
    struct mip_app_msg reply; 
    struct timespec start; 
    struct timespec end; 
    struct timeval timeout;
    double rtt_ms; 

   
    if(argc < 4){
        fprintf(stderr, "Usage: %s <socket_path> <message> <destination_mip>\n", argv[0]);
        return EXIT_FAILURE;
    }

    sd = connect_to_mipd(argv[1]);
    if(sd == -1){
        return EXIT_FAILURE;
    }
    
    /*Set the destination MIP address and build the PING payload. 
    * argv[3] is received as text so atoi() converts it to an integer before storing it as a destination MIP address.
    * snprintf() builds "PING:<message>" and stores it in msg.message.
    */
    msg.dst_mip = (unsigned char)atoi(argv[3]);
    snprintf(msg.message, sizeof(msg.message), "PING:%s", argv[2]);

    /*Start RTT measurement before transmitting the request. */
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Send the completed ping request to the local mipd. */
    if(send(sd, &msg, sizeof(msg), 0) == -1){
        perror("send");
        close(sd);
        return EXIT_FAILURE;
    }

    /* Limit recv() to one second so the client can detect a ping timeout. */
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    if (setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
        close(sd);
        return EXIT_FAILURE;
    }
    
    /* Wait for the PONG reply from mipd. */
    ssize_t bytes = recv(sd, &reply, sizeof(reply), 0);
    if (bytes == -1) {

        /* EAGAIN/EWOULDBLOCK means the one-second receive timeout expired. */
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            printf("Ping timeout\n");
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

    /* Stop the timer and calculate the complete Ping/Pong round-trip time. */
    clock_gettime(CLOCK_MONOTONIC, &end);
    rtt_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("Received from MIP %u: %s\n", reply.dst_mip, reply.message);
    printf("RTT: %.3f ms\n", rtt_ms);
    
    close(sd);
    return EXIT_SUCCESS;
}