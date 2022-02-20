#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "client_info.h"
class Connection {
 private:
  Client_Info client;
  int server_fd;

 public:
  Connection(const Client_Info & ci) :
      client(ci.getID(), ci.getFD(), ci.getIP()),
      server_fd(-1) {}
  int getClientFD() const { return client.getFD(); }
  std::string getIP() const { return client.getIP(); }
  int getID() const { return client.getID(); }
  int getServerFD() const { return server_fd; }
  void setServerFD(int fd) { server_fd = fd; }
  ~Connection() {
    close(client.getFD());
    if (server_fd != -1) {
      close(server_fd);
    }
  }
};
