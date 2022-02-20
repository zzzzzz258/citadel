#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>

int build_server(const char * port);
int build_client(const char * hostname, const char * port);
int server_accept(int socket_fd, std::string & ip);
