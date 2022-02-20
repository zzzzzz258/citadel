#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>

#include "connection.h"
#include "pthread.h"
#include "request.h"
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
  static void handle(Client_Info info);
  static void handleConnect(Connection & connection, int server_fd, Request & request);
  static void handleGet(Connection & connection,
                        const Request & request,
                        char * req_msg,
                        int len);
  static void handleGetResp(Connection & connection, const Request & request);
  static void handlePOST(Connection & connection,
                         const Request & request,
                         char * req_msg,
                         int len);
  static std::string sendContentLen(int send_fd,
                                    char * server_msg,
                                    int mes_len,
                                    int content_len);
  static int getLength(char * server_msg, int mes_len);
  static std::string getTime();
  static bool checkNotExpired(Connection & c, const Request & req, Response & rep);
  static void printcache();
  static void printcachelog(Response & parse_res,
                            bool no_store,
                            std::string req_line,
                            int id);
  static void printnote(Response & parse_res, int id);
  static void sendReqAndHandleResp(Connection & connection,
                                   const Request & request,
                                   char * req_msg,
                                   int len);
  static bool passMessage(int server_fd,
                          int client_fd,
                          char * buffer,
                          size_t buffer_size);
  static void sendResponse(Response & res, const Connection & connection);
  static bool revalidate(Response & rep, std::string raw_content, const Connection & c);
  static bool compareExpiration(int expiration_time,
                                Connection & con,
                                Response & rep,
                                const Request & request);

  static time_t getCurrentUTCTime();

  //newly added, still in testing
  static void printLog(int id = -1,
                       std::string content_1 = "",
                       std::string ip = "",
                       std::string content_2 = "");
};
