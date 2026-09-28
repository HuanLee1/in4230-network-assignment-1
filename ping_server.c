#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"


/*
* Connects the ping server to the local MIPD using a UNIX socket.
*
* socket_path: Path to the MIPD's UNIX socket.
*
* Returns the connected socker descriptor, or -1 on failure. 
*/
static int connect_to_mipd(const char *socket_path){
    struct sockaddr_un addr;
    int sd;

    /* Create the UNIX socket used to communicate with the local MIP daemon. */
    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    if (sd == -1) {
        perror("socket");
        return -1;
    }

    /*Clear address structgure before setting the UNIX socket fields.*/
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /*Stores the MIPD UNIX socket path in the address structure.*/
    strncpy( addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sd);
        return -1;
    }

    return sd;
}

/*
* Runs the ping server application.
*
* The server connects to the local MIP daemon, waits for incoming PING messages, prints them, and replies to the sender with "PONG:<message>".
*
* argc/argv: Command-line arguments containg the UNIX socket path.
* 
* Returns EXIT_SUCCESS on normal termination.
* Returns EXIT_FAILURE if setup or transmission fails. 
*/
int main(int argc, char *argv[]){
    int sd;
    struct mip_app_msg msg;
    struct mip_app_msg reply;

    /*Require the UNIX socket path to be used if you want to connect to the local MIPD.*/
    if (argc < 2){
        fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*Connect the ping server ot the local MIPD*/
    sd = connect_to_mipd(argv[1]);
    if (sd == -1) {
        return EXIT_FAILURE;
    }
    
    printf("Ping server connected to mipd\n");

    /*While(1): Forces it to keep serving requests untill the MIPD connection is closed*/
    while(1){
        ssize_t bytes = recv(sd, &msg, sizeof(msg), 0);
        if (bytes == -1) {
            perror("recv");
            break;
        }
        if (bytes == 0) {
            break;
        }

        printf("Received from MIP %u: %s\n", msg.dst_mip, msg.message);

        /*Using the same MIP address the message came from as the destination for the reply.*/
        reply.dst_mip = msg.dst_mip;
       
        /* Remove "PING:" so the reply becomes "PONG:<message>". */
        const char *prefix = "PING:";
        const char *received = msg.message;
        if(strncmp(received, prefix, strlen(prefix)) == 0){
            received += strlen(prefix);
        }

        /* Store "PONG:<received message>" in reply.message without overflowing the buffer. */
        snprintf(reply.message, sizeof(reply.message), "PONG:%.250s", received);
        if(send(sd, &reply, sizeof(reply), 0) == -1){
            perror("send");
            close(sd);
            return EXIT_FAILURE;
        }
    }
   
    close(sd);
    return EXIT_SUCCESS;
}