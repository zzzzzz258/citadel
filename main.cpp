#include <pthread.h>

#include "function.h"
#include "proxy.h"
int main() {
  const char * port = "12345";
  Proxy * myproxy = new Proxy(port);
  myproxy->run();
  return 1;
}
