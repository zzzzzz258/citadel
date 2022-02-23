#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

/**
 * Store basic information of connected client
 */
class Client_Info {
 protected:
  int id;
  int fd;
  std::string ip;

 public:
  Client_Info(int _id, int _fd, const std::string & _ip) : id(_id), fd(_fd), ip(_ip) {}

  void setFd(int my_fd) { fd = my_fd; }
  int getFD() const { return fd; }
  void setIP(std::string myip) { ip = myip; }
  std::string getIP() const { return ip; }
  void setID(int myid) { id = myid; }
  int getID() const { return id; }
};
