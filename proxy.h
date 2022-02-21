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
 public:
  Proxy(const char * myport) :
      port_num(myport)
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
  static  void respond400(const Connection & connection) ;
  static std::string sendContentLen(int send_fd,
                                    char * server_msg,
                                    int mes_len,
                                    int content_len);
  static int getLength(char * server_msg, int mes_len);
  static std::string getTime();
  static bool checkNotExpired(Connection & c, const Request & req, Response & rep);
  static void checkAndCache(Response & parse_res, std::string req_line, int id);
  static void printCacheControls(Response & parse_res, int id);
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
  static void printLog(int id = -1,
                       std::string content_1 = "",
                       std::string ip = "",
                       std::string content_2 = "");
};
