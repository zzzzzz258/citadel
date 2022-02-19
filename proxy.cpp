#include "proxy.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client_info.h"
#include "function.h"

std::mutex mtx;
std::ofstream logFile("proxy.log");

std::unordered_map<std::string, Response> cache;

void Proxy::run() {
  int temp_fd = build_server(this->port_num);
  if (temp_fd == -1) {
    //
    printLog(-1, "(no-id): ERROR in creating socket to accept");
    return;
  }
  int client_fd;
  int id = 0;
  while (1) {
    std::string ip;
    client_fd = server_accept(temp_fd, &ip);
    if (client_fd == -1) {
      //
      printLog(-1, "(no-id): ERROR in connecting client");
      continue;
    }
    Client_Info * client_info = new Client_Info(id, client_fd, ip);
    id++;
    std::thread(handle, client_info).detach();
  }
}

void * Proxy::handle(void * info) {
  Client_Info * client_info = (Client_Info *)info;
  int client_fd = client_info->getFd();

  char req_msg[65536] = {0};
  int len = recv(client_fd, req_msg, sizeof(req_msg), 0);  // fisrt request from client
  if (len <= 0) {
    //
    printLog(client_info->getID(), ": WARNING Invalid Request");
    return NULL;
  }
  std::string input = std::string(req_msg, len);
  if (input == "" || input == "\r" || input == "\n" || input == "\r\n") {
    return NULL;
  }

  Request * request = new Request(input);

  if (request->method != "POST" && request->method != "GET" &&
      request->method != "CONNECT") {
    //
    printLog(client_info->getID(), ": Unsupported request method " + request->method);

    close(client_fd);
    return NULL;
  }
  //newly added, need test
  printLog(client_info->getID(),
           ": \"" + request->start_line + "\" from ",
           client_info->getIP(),
           " @ " + getTime().append("\0"));

  std::cout << "received client request is:" << req_msg << "end" << std ::endl;
  const char * host = request->host.c_str();
  const char * port = request->port.c_str();
  std::cout << host << ":" << port << std::endl;
  int server_fd = build_client(host, port);  //connect to server
  if (server_fd == -1) {
    std::cout << "Error in build client!\n";
    return NULL;
  }

  if (request->method == "CONNECT") {  // handle connect request
    //
    printLog(client_info->getID(),
             ": Requesting \"" + request->start_line + "\" from " + host);
    handleConnect(client_fd, server_fd, client_info->getID());
    //
    printLog(client_info->getID(), ": Tunnel closed");
  }

  else if (request->method == "GET") {  //handle get request
    int id = client_info->getID();      // thread id for logging
    std::unordered_map<std::string, Response>::iterator it = cache.begin();
    it = cache.find(request->start_line);
    if (it == cache.end()) {  // request not found in cache
      //
      printLog(client_info->getID(), ": not in cache");
      //
      sendReqAndHandleResp(
          id, request->start_line, req_msg, len, client_fd, server_fd, host);
    }
    else {  //request found in cache
      if (request->no_cache ||
          it->second.no_cache) {  //has no-cache symbol, revalidate all the time
        //
        printLog(client_info->getID(), ": in cache, requires validation cuz no-cache");

        if (revalidation(it->second, request->raw_content, server_fd, id) ==
            false) {  //check Etag and Last Modified
          sendReqAndHandleResp(
              id, request->start_line, req_msg, len, client_fd, server_fd, host);
        }
        else {
          sendCachedResp(it->second, id, client_fd);
        }
      }
      else {
        bool valid = CheckTime(
            server_fd, *request, request->start_line, it->second, client_info->getID());

        if (!valid) {  //ask for server,check res and put in cache if needed
          sendReqAndHandleResp(
              id, request->start_line, req_msg, len, client_fd, server_fd, host);
        }
        else {  //send from cache
          sendCachedResp(it->second, id, client_fd);
        }
      }
    }
  }
  else if (request->method == "POST") {  //handle post request
    //
    printLog(client_info->getID(),
             ": Requesting \"" + request->start_line + "\" from " + host);
    handlePOST(client_fd, server_fd, req_msg, len, client_info->getID(), host);
  }
  close(server_fd);
  close(client_fd);
  return NULL;
}

/**
 * Send request message to server, and handle its response
 */
void Proxy::sendReqAndHandleResp(int id,
                                 std::string start_line,
                                 char * req_msg,
                                 int len,
                                 int client_fd,
                                 int server_fd,
                                 const char * host) {
  printLog(id, ": Requesting \"" + start_line + "\" from " + std::string(host));
  send(server_fd, req_msg, len, 0);
  handleGet(client_fd, server_fd, id, host, start_line);
}

/**
 * use cached response, send it back to client
 * @param res is the cached response
 */
void Proxy::sendCachedResp(Response & res, int id, int client_fd) {
  char cache_res[res.getSize()];
  const std::vector<char> & response_content = res.getRawContent();
  auto it = response_content.begin();
  for (int i = 0; i < res.getSize(); i++, it++) {
    cache_res[i] = *it;
  }
  send(client_fd, cache_res, res.getSize(), 0);
  //
  printLog(id, ": Requesting \"" + res.start_line + "\"");
}

/**
 * Check if the cached resposne expires
 * @return false if expires or revalidation failed(new request), true use cache
 */
bool Proxy::CheckTime(int server_fd,
                      Request & request,
                      std::string req_start_line,
                      Response & rep,
                      int id) {
  if (rep.max_age != -1) {
    time_t curr_time = time(0);
    time_t rep_time = mktime(rep.response_time.getTimeStruct());
    int max_age = rep.max_age;
    if (rep_time + max_age <= curr_time) {  // stale
      if (rep.must_revalidate) {            // validation required
        mtx.lock();
        logFile << id << ": in cache, requires validation cuz must-validate" << std::endl;
        mtx.unlock();
        return revalidation(rep, request.raw_content, server_fd, id);
      }
      cache.erase(req_start_line);
      mtx.lock();
      time_t dead_time = mktime(rep.expire_time.getTimeStruct());
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      //
      printLog(id, ": in cache, but expired at " + std::string(t));
      return false;
    }
  }
  else if (rep.exp_str != "") {
    time_t curr_time = time(0);
    time_t expire_time = mktime(rep.expire_time.getTimeStruct());
    if (curr_time > expire_time) {
      if (rep.must_revalidate) {  // validation required
        mtx.lock();
        logFile << id << ": in cache, requires validation cuz must-validate" << std::endl;
        mtx.unlock();
        return revalidation(rep, request.raw_content, server_fd, id);
      }
      cache.erase(req_start_line);
      time_t dead_time = mktime(rep.expire_time.getTimeStruct());
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      //
      printLog(id, ": in cache, but expired at " + std::string(t));

      return false;
    }
  }
  /*
  bool revalid = revalidation(rep, request.input, server_fd, id);
  if (revalid == false) {
    return false;
  }
  */
  //
  printLog(id, ": in cache, valid");
  // no cache-control fields define an expiration time, cache valid

  return true;
}

/**
 * A function to check if revalidation is necessary
 * @return: true if revalidate successfullly, false for new request
 */
bool Proxy::revalidation(Response & rep, std::string raw_content, int server_fd, int id) {
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
  if ((send_len = send(server_fd, req_new_msg, req_msg_str.size() + 1, 0)) >
      0) {  // send request with validator
    std::cout << "Verify: Send success!\n";
  }
  char new_resp[65536] = {0};
  int new_len = recv(server_fd, &new_resp, sizeof(new_resp), 0);
  if (new_len <= 0) {
    std::cout << "[Verify] received from server failed in checktime" << std::endl;
  }
  std::string checknew(new_resp, new_len);
  if (checknew.find("HTTP/1.1 200 OK") != std::string::npos) {  //received a new response
    return false;
  }
  return true;  //use from cache
}

/**
 * A handler of Post request
 */
void Proxy::handlePOST(int client_fd,
                       int server_fd,
                       char * req_msg,
                       int len,
                       int id,
                       const char * host) {
  int post_len = getLength(req_msg, len);  //get length of client request
  if (post_len != -1) {
    std::string request = sendContentLen(client_fd, req_msg, len, post_len);
    char send_request[request.length() + 1];
    strcpy(send_request, request.c_str());
    send(server_fd,
         send_request,
         sizeof(send_request),
         MSG_NOSIGNAL);  // send all the request info from client to server
    char response[65536] = {0};
    int response_len = recv(server_fd,
                            response,
                            sizeof(response),
                            MSG_WAITALL);  //first time received response from server
    if (response_len != 0) {
      Response res;
      res.parseStartLine(req_msg, len);
      //
      printLog(id, ": Received \"" + res.getStartLine() + "\" from " + host);

      std::cout << "receive response from server which is:" << response << std::endl;

      send(client_fd, response, response_len, MSG_NOSIGNAL);
      //
      printLog(id, ": Responding \"" + res.getStartLine());
    }
    else {
      std::cout << "server socket closed!\n";
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

void Proxy::handleGet(int client_fd,
                      int server_fd,
                      int id,
                      const char * host,
                      std::string req_start_line) {
  char server_msg[65536] = {0};
  int mes_len = recv(server_fd,
                     server_msg,
                     sizeof(server_msg),
                     0);  //received first response from server(all header, part body)
  //TEST
  std::string temp(server_msg, 300);
  std::cout << "Receive server response is: " << temp << std::endl;
  //TEST END
  if (mes_len == 0) {
    return;
  }
  Response response;
  response.parseStartLine(server_msg,
                          mes_len);  // parse and get the first start_line
  response.setRawContent(std::string(server_msg, mes_len));

  //
  printLog(id, ": Received \"" + response.getStartLine() + "\" from " + host);

  bool is_chunk = findChunk(server_msg, mes_len);
  if (is_chunk) {  // chunked response, no cache, just resend
    //
    printLog(id, ": not cacheable because it is chunked");

    send(client_fd, server_msg, mes_len, 0);  //send first response to server
    char chunked_msg[28000] = {0};
    while (1) {  //receive and send remaining message
      if (!passMessage(server_fd, client_fd, chunked_msg, sizeof(chunked_msg))) {
        std::cout << "chunked break" << std::endl;
        break;
      }
    }
  }
  else {
    std::string server_msg_str(server_msg, mes_len);
    // checking no-store header
    bool no_store = false;
    size_t nostore_pos;
    if ((nostore_pos = server_msg_str.find("no-store")) != std::string::npos) {
      no_store = true;
    }
    response.parseField(server_msg, mes_len);          // fill attributes of response
    printnote(response, id);                           // print cache related note
    int content_len = getLength(server_msg, mes_len);  //get content length
    if (content_len != -1) {                           // content_len specified
      std::string msg = sendContentLen(
          server_fd, server_msg, mes_len, content_len);  //get the entire message
      // send response to client
      std::vector<char> large_msg;
      for (size_t i = 0; i < msg.length(); i++) {
        large_msg.push_back(msg[i]);
      }
      const char * send_msg = large_msg.data();
      response.setRawContent(large_msg);
      send(client_fd, send_msg, msg.length(), 0);
    }
    else {  // content-length not specified, take it as whole message has been received
      std::string server_msg_str(server_msg, mes_len);
      response.setRawContent(server_msg_str);
      send(client_fd, server_msg, mes_len, 0);
    }
    printcachelog(response, no_store, req_start_line, id);
  }
  // print messages
  std::cout << "Responding for GET\n";
  std::string logrespond(server_msg, mes_len);
  size_t log_pos = logrespond.find_first_of("\r\n");

  //problem remaining
  std::string log_start_line = logrespond.substr(0, log_pos);
  //
  std::cout << "logfile responding\n";
  //
  printLog(id, ": Responding \"" + log_start_line + "\"");
}

void Proxy::Check502(std::string entire_msg, int client_fd, int id) {
  if (entire_msg.find("\r\n\r\n") == std::string::npos) {
    const char * bad502 = "HTTP/1.1 502 Bad Gateway";
    send(client_fd, bad502, sizeof(bad502), 0);
    //
    printLog(id, ": Responding \"HTTP/1.1 502 Bad Gateway\"");
  }
}

void Proxy::printnote(Response & response, int id) {
  if (response.max_age != -1) {
    //C++ 11 only
    printLog(id, ": NOTE Cache-Control: max-age=" + std::to_string(response.max_age));
  }
  if (response.exp_str != "") {
    //
    printLog(id, ": NOTE Expires: " + response.exp_str);
  }
  if (response.no_cache == true) {
    printLog(id, "NOTE Cache-Control: no-cache");
  }
  if (response.etag != "") {
    //
    printLog(id, ": NOTE etag: " + response.etag);
  }
  if (response.lastModified != "") {
    //
    printLog(id, ": NOTE Last-Modified: " + response.lastModified);
  }
}

void Proxy::printcachelog(Response & response,
                          bool no_store,
                          std::string req_start_line,
                          int id) {
  //
  printLog(id, ": function printcachelog called");

  if (response.getRawContentString(100).find("HTTP/1.1 200 OK") !=
      std::string::npos) {  // cacheable response
    if (no_store) {         // no-store specified
      //
      printLog(id, ": not cacheable because NO STORE");
      return;
    }
    if (response.max_age != -1) {  // max-age specified
      time_t dead_time =
          mktime(response.response_time.getTimeStruct()) + response.max_age;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      //
      printLog(id, ": cached, expires at " + std::string(t));
    }
    else if (response.exp_str != "") {
      //
      printLog(id, ": cached, expires at " + response.exp_str);
    }
    // not dealing with the situation that neither max-age nor expired-time is sprcified
    Response storedres(response);
    if (cache.size() > 10) {
      std::unordered_map<std::string, Response>::iterator it = cache.begin();
      cache.erase(it);
    }
    cache.insert(std::pair<std::string, Response>(req_start_line, storedres));
    //
    printLog(id, ": ADD NEW item to cache");
  }
  //
  printLog(
      id,
      ": HTTP/1.1 200 OK not found in response, not cache it" + response.lastModified);
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

bool Proxy::findChunk(char * server_msg, int mes_len) {
  std::string msg(server_msg, mes_len);
  size_t pos;
  if ((pos = msg.find("chunked")) != std::string::npos) {
    return true;
  }
  return false;
}

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

void Proxy::handleConnect(int client_fd, int server_fd, int id) {
  send(client_fd, "HTTP/1.1 200 OK\r\n\r\n", 19, 0);
  //
  printLog(id, ": Responding \"HTTP/1.1 200 OK\"");

  fd_set readfds;
  int nfds = server_fd > client_fd ? server_fd + 1 : client_fd + 1;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    FD_SET(client_fd, &readfds);

    select(nfds, &readfds, NULL, NULL, NULL);
    int fd[2] = {server_fd, client_fd};
    for (int i = 0; i < 2; i++) {
      char buffer[65536] = {0};
      if (FD_ISSET(fd[i], &readfds)) {
        if (!passMessage(fd[i], fd[1 - i], buffer, sizeof(buffer))) {
          return;
        }
      }
    }
  }
}

std::string Proxy::getTime() {
  time_t currTime = time(0);
  struct tm * nowTime = gmtime(&currTime);
  const char * t = asctime(nowTime);
  return std::string(t);
}

//newly added
void Proxy::printLog(int id,
                     std::string content_1,
                     std::string ip,
                     std::string content_2) {
  mtx.lock();
  //
  if (id == -1) {
    logFile << content_1 << std::endl;
  }
  if (id != -1 && ip == "") {
    logFile << id << content_1 << std::endl;
  }
  if (id != -1 && ip != "") {
    logFile << id << content_1 << ip << content_2 << std::endl;
  }
  //
  mtx.unlock();
}
