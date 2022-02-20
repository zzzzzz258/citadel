#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "client_info.h"
class Client : public Client_Info {
 public:
  Client(const Client_Info & ci) : Client_Info(ci.getID(), ci.getFD(), ci.getIP()) {}
  ~Client() { close(fd); }
};
