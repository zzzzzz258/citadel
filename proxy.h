#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>

#include "parse.h"
#include "pthread.h"
#include "response.h"
class Proxy {
 private:
  const char * port_num;
  //  std::ofstream logFile;
  //  pthread_mutex_t mutex;
  //  std::unordered_map<std::string, Response> cache;

 public:
  Proxy(const char * myport) :
      port_num(myport)
  //logFile(logAddr),
  //mutex(PTHREAD_MUTEX_INITIALIZER)
  {}
  void run();
  static void * handle(void * arg);
  static void handleConnect(int client_fd, int server_fd, int id);
  static void handleGet(int client_fd,
                        int server_fd,
                        int id,
                        const char * host,
                        std::string req_line);
  static void handlePOST(int client_fd,
                         int server_fd,
                         char * req_msg,
                         int len,
                         int id,
                         const char * host);
  static std::string sendContentLen(int send_fd,
                                    char * server_msg,
                                    int mes_len,
                                    int content_len);
  static int getLength(char * server_msg, int mes_len);
  static bool findChunk(char * server_msg, int mes_len);
  static std::string getTime();
  static bool CheckTime(int server_fd,
                        Request & parser,
                        std::string req_line,
                        Response & rep,
                        int id);
  static void printcache();
  static void printcachelog(Response & parse_res,
                            bool no_store,
                            std::string req_line,
                            int id);
  static void printnote(Response & parse_res, int id);
  static void ask_server(int id,
                         std::string line,
                         char * req_msg,
                         int len,
                         int client_fd,
                         int server_fd,
                         const char * host);
  static void use_cache(Response & res, int id, int client_fd);
  static bool revalidation(Response & rep, std::string input, int server_fd, int id);
  static void Check502(std::string entire_msg, int client_fd, int id);

  //newly added, still in testing
  static void printLog(int id = -1,
                       std::string content_1 = "",
                       std::string ip = "",
                       std::string content_2 = "");
};
