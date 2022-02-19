#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

/**
 * Store basic information of connected client
 */
class Client_Info {
 private:
  int id;
  int client_fd;
  std::string ip;

 public:
  Client_Info(int _id, int _fd, std::string & _ip) : id(_id), client_fd(_fd), ip(_ip) {}
  void setFd(int my_client_fd) { client_fd = my_client_fd; }
  int getFd() { return client_fd; }
  void setIP(std::string myip) { ip = myip; }
  std::string getIP() { return ip; }
  void setID(int myid) { id = myid; }
  int getID() { return id; }
};
