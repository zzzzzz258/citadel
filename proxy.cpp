#include "proxy.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <ctime>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "function.h"

std::mutex mtx_log;
std::mutex mtx_cache;
std::ofstream logFile("proxy.log");

std::unordered_map<std::string, Response> cache;

void Proxy::run() {
  int temp_fd = build_server(this->port_num);
  if (temp_fd == -1) {
    printLog(-1, "(no-id): ERROR in creating socket to accept");
    return;
  }
  int client_fd;
  int id = 0;
  while (1) {
    std::string ip;
    try {
      client_fd = server_accept(temp_fd, ip);
    }
    catch (std::exception & e) {
      printLog(-1, "(no-id): ERROR in connecting client");
      continue;
    }
    Client_Info client_info = Client_Info(id, client_fd, ip);
    id++;
    std::thread(handle, std::ref(client_info)).detach();
  }
}

/**
 * @param info is a passenger of information about client
 */
void Proxy::handle(Client_Info info) {
  Connection connection(info);
  char req_msg[65536] = {0};
  int len = recv(connection.getClientFD(),
                 req_msg,
                 sizeof(req_msg),
                 0);  // receive first request from client
  if (len <= 2) {
    printLog(connection.getID(), ": WARNING Invalid Request");
    return;
  }

  Request request = Request(std::string(req_msg, len));
  if (!request.solvable()) {  // just shut connect for unsupported methods
    printLog(connection.getID(), ": Unsupported request method " + request.method);
    return;
  }

  printLog(connection.getID(),
           ": \"" + request.start_line + "\" from ",
           connection.getIP(),
           " @ " + getTime().append("\0"));

  std::cout << "received client request is:\n" << req_msg << std ::endl;
  const char * host = request.host.c_str();
  const char * port = request.port.c_str();
  std::cout << host << ":" << port << std::endl;

  int server_fd;
  try {
    server_fd = build_client(host, port);  //connect to server
    connection.setServerFD(server_fd);
  }
  catch (std::exception & e) {
    printLog(connection.getID(), std::string(": NOTE ") + e.what());
    return;
  }
  if (request.method == "CONNECT") {  // handle connect request
    handleConnect(connection, server_fd, request);
  }
  else if (request.method == "GET") {  //handle get request
    handleGet(connection, request, req_msg, len);
  }
  else if (request.method == "POST") {  //handle post request
    handlePOST(connection, request, req_msg, len);
  }
  return;
}

void Proxy::handleGet(Connection & connection,
                      const Request & request,
                      char * req_msg,
                      int len) {
  std::unordered_map<std::string, Response>::iterator it = cache.begin();
  it = cache.find(request.start_line);
  if (it == cache.end()) {  // request not found in cache
    printLog(connection.getID(), ": not in cache");
    sendReqAndHandleResp(connection, request, req_msg, len);
  }
  else {  //request found in cache
    if (request.no_cache || it->second.no_cache) {
      printLog(connection.getID(), ": in cache, requires validation");
      if (revalidate(it->second, request.raw_content, connection) == false) {
        sendReqAndHandleResp(connection, request, req_msg, len);
      }  //check Etag and Last Modified
      else {
        sendResponse(it->second, connection);
      }
    }  //has no-cache symbol, revalidate all the time
    else {
      if (!checkNotExpired(connection, request, it->second)) {
        sendReqAndHandleResp(connection, request, req_msg, len);
      }  // expired, or must-revalidate filed
      else {
        sendResponse(it->second, connection);
      }  //not expired, or revalidated, send from cache
    }
  }
}

/**
 * Send request message to server, and handle its response
 */
void Proxy::sendReqAndHandleResp(Connection & connection,
                                 const Request & request,
                                 char * req_msg,
                                 int len) {
  printLog(
      connection.getID(),
      ": Requesting \"" + request.start_line + "\" from " + std::string(request.host));
  send(connection.getServerFD(), req_msg, len, 0);
  handleGetResp(connection, request);
}

/**
 * use cached response, send it back to client
 * @param res is the cached response
 */
void Proxy::sendResponse(Response & res, const Connection & connection) {
  char cache_res[res.getSize()];
  const std::vector<char> & response_content = res.getRawContent();
  auto it = response_content.begin();
  for (int i = 0; i < res.getSize(); i++, it++) {
    cache_res[i] = *it;
  }
  send(connection.getClientFD(), cache_res, res.getSize(), 0);
  printLog(connection.getID(), ": Responding \"" + res.start_line + "\"");
}

bool Proxy::compareExpiration(int expiration_time,
                              Connection & con,
                              Response & rep,
                              const Request & request) {
  time_t curr_time;  // this time is in current time zone
  time(&curr_time);
  curr_time += 5 * 60 * 60;
  if (expiration_time <= curr_time) {  // stale
    if (rep.must_revalidate) {         // validation required
      printLog(con.getID(), ": in cache, requires validation");
      return revalidate(rep, request.raw_content, con);
    }
    cache.erase(request.start_line);
    time_t dead_time = mktime(rep.expire_time.getTimeStruct());
    struct tm * asc_time = gmtime(&dead_time);
    const char * t = asctime(asc_time);
    printLog(con.getID(), ": in cache, but expired at " + std::string(t));
    return false;
  }
  return true;
}

/**
 * Check if the cached resposne expires
 * @return false if expires or revalidate failed(new request), true use cache
 */
bool Proxy::checkNotExpired(Connection & connection,
                            const Request & request,
                            Response & rep) {
  if (rep.max_age != -1) {
    time_t rep_time = mktime(rep.response_time.getTimeStruct());
    int max_age = rep.max_age;
    if (!compareExpiration(rep_time + max_age, connection, rep, request)) {
      return false;
    }
  }
  else if (rep.exp_str != "") {
    time_t expire_time = mktime(rep.expire_time.getTimeStruct());
    if (!compareExpiration(expire_time, connection, rep, request)) {
      return false;
    }
  }
  printLog(connection.getID(), ": in cache, valid");
  // no cache-control fields define an expiration time, cache valid
  return true;
}
/**
 * A function to check if revalidate is necessary
 * @return: true if revalidate successfullly, false for new request
 */
bool Proxy::revalidate(Response & rep, std::string raw_content, const Connection & c) {
  if (rep.etag == "" && rep.lastModified == "") {  // no validator available
    return true;
  }
  std::string changed_raw_content = raw_content;
  if (rep.etag != "") {
    std::string add_etag = "If-None-Match: " + rep.etag.append("\r\n");
    changed_raw_content =
        changed_raw_content.insert(changed_raw_content.length() - 2, add_etag);
  }
  if (rep.lastModified != "") {
    std::string add_modified = "If-Modified-Since: " + rep.lastModified.append("\r\n");
    changed_raw_content =
        changed_raw_content.insert(changed_raw_content.length() - 2, add_modified);
  }
  std::string req_msg_str = changed_raw_content;
  //  char req_new_msg[req_msg_str.size() + 1];
  const char * req_new_msg = changed_raw_content.c_str();
  int send_len;
  if ((send_len = send(c.getServerFD(), req_new_msg, req_msg_str.size() + 1, 0)) >
      0) {  // send request with validator
    std::cout << "Verify: Send success!\n";
  }
  char new_resp[65536] = {0};
  int new_len = recv(c.getServerFD(), &new_resp, sizeof(new_resp), 0);
  if (new_len <= 0) {
    std::cout << "[Verify] received from server failed in checktime" << std::endl;
  }
  std::string checknew(new_resp, new_len);
  if (checknew.find("304 Not Modified") != std::string::npos) {  // validate success
    printLog(c.getID(), ": NOTE revalidate successfullly");
    return true;
  }
  return false;
}

/**
 * A handler of Post request
 */
void Proxy::handlePOST(Connection & connection,
                       const Request & request,
                       char * req_msg,
                       int len) {
  printLog(connection.getID(),
           ": Requesting \"" + request.start_line + "\" from " + request.host);
  int post_len = getLength(req_msg, len);  //get length of client request
  if (post_len != -1) {
    std::string full_request =
        sendContentLen(connection.getClientFD(), req_msg, len, post_len);
    char send_request[full_request.length() + 1];
    strcpy(send_request, full_request.c_str());
    send(connection.getServerFD(),
         send_request,
         sizeof(send_request),
         MSG_NOSIGNAL);  // send all the request info from client to server
    char response[65536] = {0};
    int response_len = recv(connection.getServerFD(),
                            response,
                            sizeof(response),
                            MSG_WAITALL);  //first time received response from server
    if (response_len != 0) {
      Response res;
      res.parseStartLine(req_msg, len);
      printLog(connection.getID(),
               ": Received \"" + res.getStartLine() + "\" from " + request.host);

      std::cout << "receive response from server which is:" << response << std::endl;

      send(connection.getClientFD(), response, response_len, MSG_NOSIGNAL);
      printLog(connection.getID(), ": Responding \"" + res.getStartLine());
    }
    else {
      printLog(connection.getID(), ": ERROR scoket unexpectedly closed");
    }
  }
}

/**
 * Transparently pass data from server to client (or inverse)
 * @return false means error occured in the process
 */
bool Proxy::passMessage(int server_fd, int client_fd, char * buffer, size_t buffer_size) {
  int len = recv(server_fd, buffer, buffer_size, 0);
  if (len <= 0) {
    std::cout << "chunked break\n";
    return false;
  }
  if (send(client_fd, buffer, len, 0) <= 0) {
    return false;
  }
  return true;
}

void Proxy::handleGetResp(Connection & connection, const Request & request) {
  char server_msg[65536] = {0};
  int mes_len = recv(connection.getServerFD(),
                     server_msg,
                     sizeof(server_msg),
                     0);  //received first response from server(all header, part body)
  //TEST
  std::string temp(server_msg, 300);
  std::cout << "Receive server response is: " << temp << std::endl;
  //TEST END
  Response response;
  if (mes_len == 0) {
    std::string resp502("502 Bad Gateway");
    response.parseStartLine(resp502.c_str(), resp502.length());
    response.setRawContent(resp502);
    sendResponse(response, connection);
    return;
  }
  response.parseStartLine(server_msg,
                          mes_len);  // parse and get the first start_line
  response.setRawContent(std::string(server_msg, mes_len));
  response.parseField(server_msg, mes_len);  // fill attributes of response
  printLog(connection.getID(),
           ": Received \"" + response.getStartLine() + "\" from " + request.host);
  if (response.chunked) {  // chunked response, no cache, just resend
    printLog(connection.getID(), ": not cacheable because it is chunked");

    send(connection.getClientFD(),
         server_msg,
         mes_len,
         0);  //send first response to server
    char chunked_msg[28000] = {0};
    while (1) {  //receive and send remaining message
      if (!passMessage(connection.getServerFD(),
                       connection.getClientFD(),
                       chunked_msg,
                       sizeof(chunked_msg))) {
        printLog(connection.getID(), ": NOTE chunked break");
        break;
      }
    }
  }
  else {
    std::string server_msg_str(server_msg, mes_len);
    printnote(response, connection.getID());           // print cache related note
    int content_len = getLength(server_msg, mes_len);  //get content length
    if (content_len != -1) {                           // content_len specified
      std::string msg = sendContentLen(connection.getServerFD(),
                                       server_msg,
                                       mes_len,
                                       content_len);  //get the entire message
      // send response to client
      std::vector<char> large_msg;
      for (size_t i = 0; i < msg.length(); i++) {
        large_msg.push_back(msg[i]);
      }
      const char * send_msg = large_msg.data();
      response.setRawContent(large_msg);
      send(connection.getClientFD(), send_msg, msg.length(), 0);
    }
    else {  // content-length not specified, take it as whole message has been received
      send(connection.getClientFD(), server_msg, mes_len, 0);
    }
    printcachelog(response, request.start_line, connection.getID());
  }
  printLog(connection.getID(), ": Responding \"" + response.start_line + "\"");
}

void Proxy::printnote(Response & response, int id) {
  if (response.max_age != -1) {
    //C++ 11 only
    printLog(id, ": NOTE Cache-Control: max-age=" + std::to_string(response.max_age));
  }
  if (response.exp_str != "") {
    printLog(id, ": NOTE Expires: " + response.exp_str);
  }
  if (response.no_cache == true) {
    printLog(id, "NOTE Cache-Control: no-cache");
  }
  if (response.no_store == true) {
    printLog(id, "NOTE Cache-Control: no-store");
  }
  if (response.etag != "") {
    printLog(id, ": NOTE etag: " + response.etag);
  }
  if (response.lastModified != "") {
    printLog(id, ": NOTE Last-Modified: " + response.lastModified);
  }
}

void Proxy::printcachelog(Response & response, std::string req_start_line, int id) {
  printLog(id, ": NOTE function printcachelog called");

  if (response.getRawContentString(100).find("HTTP/1.1 200 OK") !=
      std::string::npos) {    // cacheable response
    if (response.no_store) {  // no-store specified
      printLog(id, ": not cacheable because NO STORE");
      return;
    }
    else if (response.no_cache) {
      printLog(id, ": cached, but requires re-validation");
    }
    else if (response.max_age != -1) {  // max-age specified
      time_t dead_time =
          mktime(response.response_time.getTimeStruct()) + response.max_age;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      printLog(id, ": cached, expires at " + std::string(t));
    }
    else if (response.exp_str != "") {
      printLog(id, ": cached, expires at " + response.exp_str);
    }
    else {
      printLog(id,
               ": not cacheable because no valid expiration time or revalidation rule is "
               "specified");
      return;
    }
    // not dealing with the situation that neither max-age nor expired-time is sprcified
    //    Response storedres(response);
    mtx_cache.lock();
    if (cache.size() > 10) {
      std::unordered_map<std::string, Response>::iterator it = cache.begin();
      cache.erase(it);
    }
    cache.insert(std::pair<std::string, Response>(req_start_line, response));
    mtx_cache.unlock();
  }
  else {
    printLog(id, ": not cacheable because HTTP/1.1 200 OK not found in response");
  }
}

std::string Proxy::sendContentLen(int send_fd,
                                  char * server_msg,
                                  int mes_len,
                                  int content_len) {
  int total_len = 0;
  int len = 0;
  std::string msg(server_msg, mes_len);

  while (total_len < content_len) {
    char new_server_msg[65536] = {0};
    if ((len = recv(send_fd, new_server_msg, sizeof(new_server_msg), 0)) <= 0) {
      break;
    }
    std::string temp(new_server_msg, len);
    msg += temp;
    total_len += len;
  }
  return msg;
}

/**
 * get content-length from request header
 */
int Proxy::getLength(char * server_msg, int mes_len) {
  std::string msg(server_msg, mes_len);
  size_t pos;
  if ((pos = msg.find("Content-Length: ")) != std::string::npos) {
    size_t head_end = msg.find("\r\n\r\n");

    int part_body_len = mes_len - static_cast<int>(head_end) - 8;
    size_t end = msg.find("\r\n", pos);
    std::string content_len = msg.substr(pos + 16, end - pos - 16);
    int num = 0;
    for (size_t i = 0; i < content_len.length(); i++) {
      num = num * 10 + (content_len[i] - '0');
    }
    return num - part_body_len - 4;
  }
  return -1;
}

void Proxy::handleConnect(Connection & connection, int server_fd, Request & request) {
  printLog(connection.getID(),
           ": Requesting \"" + request.start_line + "\" from " + request.host);

  int id = connection.getID();
  send(connection.getClientFD(), "HTTP/1.1 200 OK\r\n\r\n", 19, 0);
  printLog(id, ": Responding \"HTTP/1.1 200 OK\"");

  fd_set readfds;
  int nfds =
      server_fd > connection.getClientFD() ? server_fd + 1 : connection.getClientFD() + 1;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    FD_SET(connection.getClientFD(), &readfds);

    select(nfds, &readfds, NULL, NULL, NULL);
    int fd[2] = {server_fd, connection.getClientFD()};
    for (int i = 0; i < 2; i++) {
      char buffer[65536] = {0};
      if (FD_ISSET(fd[i], &readfds)) {
        if (!passMessage(fd[i], fd[1 - i], buffer, sizeof(buffer))) {
          return;
        }
      }
    }
  }
  printLog(connection.getID(), ": Tunnel closed");
}

std::string Proxy::getTime() {
  time_t currTime = time(0);
  struct tm * nowTime = gmtime(&currTime);
  const char * t = asctime(nowTime);
  return std::string(t);
}

time_t getCurrentUTCTime() {
  time_t curr_time;  // this time is in current time zone
  ::time(&curr_time);
  curr_time += 5 * 60 * 60;
  return curr_time;
}

void Proxy::printLog(int id,
                     std::string content_1,
                     std::string ip,
                     std::string content_2) {
  mtx_log.lock();
  if (id == -1) {
    logFile << content_1 << std::endl;
  }
  if (id != -1 && ip == "") {
    logFile << id << content_1 << std::endl;
  }
  if (id != -1 && ip != "") {
    logFile << id << content_1 << ip << content_2 << std::endl;
  }

  mtx_log.unlock();
}
