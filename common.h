
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

/*End of guard*/
#endif