
/* Guard helps prevent common.h from being included multiple times.
* Else it'll duplicate stuff during compilation.
*/
#ifndef _COMMON_H
#define _COMMON_H

/*Defining how many events epoll can report at the same time,
* and how many connections are allowed.
*/
#define MAX_EVENTS 10
#define MAX_CONNS 5


/* Represents a message sent over the UNIX socket. */
struct mip_app_msg {
    unsigned char dst_mip;
    char message[256];
};

/*End of guard*/
#endif