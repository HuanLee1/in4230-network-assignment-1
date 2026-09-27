#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

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
};


int main(int argc, char *argv[]){
    int sd;
    
    /*Input check before connecting client to MIPD UNIX socket.*/
    if(argc < 2){
        fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*Conmect the client to the local MIPD UNIX socket. */
    sd = connect_to_mipd(argv[1]);
    if(sd == -1){
        return EXIT_FAILURE;
    }

    printf("Connected to mipd\n");

    const char *message = "Hello";

    if(send(sd, message, strlen(message) +1, 0) == -1){
        perror("send");
        close(sd);
        return EXIT_FAILURE;
    }

    

    close(sd);

    return EXIT_SUCCESS;
};