#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"


static int connect_to_mipd(const char *socket_path)
{
    struct sockaddr_un addr;
    int sd;

    sd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    if (sd == -1) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy( addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (connect(sd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sd);
        return -1;
    }

    return sd;
}


int main(int argc, char *argv[]){
    int sd;

    if (argc < 2){
        fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    sd = connect_to_mipd(argv[1]);

    if (sd == -1) {
        return EXIT_FAILURE;
    }
    
    printf("Ping server connected to mipd\n");

    struct mip_app_msg msg;
  
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

        struct mip_app_msg reply;
        reply.dst_mip = msg.dst_mip;

        const char *prefix = "PING:";
        const char *received = msg.messages;

        if(strncmp(receivd, prefix, strlen(prefix)) == 0){
            received += strlen(prefix);
        }

        snprintf(reply.message, sizeof(reply.message), "PONG:%s", received);
        if(send(sd, &reply, sizeof(reply), 0) == -1){
            perror("send");
            close(sd);
            return EXIT_FAILURE;
        }
    }
   
    close(sd);
    return EXIT_SUCCESS;
}