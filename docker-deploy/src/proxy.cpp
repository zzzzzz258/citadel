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
    printLog(-1, ": ERROR in creating socket to accept");
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
      printLog(-1, ": ERROR in connecting client");
      continue;
    }
    Client_Info client_info = Client_Info(id, client_fd, ip);      
    id++;
    std::thread(handle, std::ref(client_info)).detach();
  }
}

 void Proxy::respond400(const Connection & connection) {      
   if (send(connection.getClientFD(), "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0) == -1){
     throw std::runtime_error(": ERROR send fail in respond400");
   }
    printLog(connection.getID(), ": WARNING Invalid Request");
    printLog(connection.getID(), ": Responding \"" + std::string("HTTP/1.1 400 Bad Request") + "\"");   
}

 void Proxy::respond502(const Connection & connection) {
   if (send(connection.getClientFD(), "HTTP/1.1 502 Bad Gateway\r\n\r\n", 28, 0) == -1) {
     throw std::runtime_error(": ERROR send fail in respond502");
   }
   printLog(connection.getID(), ": WARNING Invalid Response");
   printLog(connection.getID(), ": Responding \"" + std::string("HTTP/1.1 502 Bad Gateway") + "\"");   
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
  if (len < 0) {
    try{
      respond400(connection);
    } catch(std::exception & e) {
      printLog(connection.getID(), e.what());
    }
    return;
  }

  Request request = Request(std::string(req_msg, len));
  if (!request.solvable()) {  // just shut connect for unsupported methods
    try{
      respond400(connection);
    } catch(std::exception & e) {
      printLog(connection.getID(), ": NOTE request method " + request.method);
      printLog(connection.getID(), e.what());
    }
    return;
  }

  printLog(connection.getID(),
           ": \"" + request.start_line + "\" from ",
           connection.getIP(),
           " @ " + getTime());

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
}

void Proxy::handleGet(Connection & connection,
                      const Request & request,
                      char * req_msg,
                      int len) {
  Response in_cache = findCache(request.start_line);
  if (in_cache.raw_content.empty()) {  // request not found in cache
    printLog(connection.getID(), ": not in cache");
    sendReqAndHandleResp(connection, request, req_msg, len);
  }
  else {  //request found in cache    
    if (request.no_cache || in_cache.no_cache) {
      printLog(connection.getID(), ": in cache, requires validation");
      if (revalidate(in_cache, request.raw_content, connection) == false) {
        sendReqAndHandleResp(connection, request, req_msg, len);
      }  //check Etag and Last Modified
      else {
        sendResponse(in_cache, connection);
      }
    }  //has no-cache symbol, revalidate all the time
    else {
      if (!checkNotExpired(connection, request, in_cache)) {
        sendReqAndHandleResp(connection, request, req_msg, len);
      }  // expired, or must-revalidate failesd
      else {
        sendResponse(in_cache, connection);
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
  int slen = send(connection.getServerFD(), req_msg, len, 0);
  if (slen == -1) {
    printLog(connection.getID(), ": ERROR failure in sending request to server");
    return;
  }
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
  int slen = send(connection.getClientFD(), cache_res, res.getSize(), 0);
  if (slen == -1) {
    printLog(connection.getID(), ": ERROR failure in sending response from proxy to client");
    return;
  }
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
    removeCache(request.start_line);
    time_t dead_time = mktime(rep.expire_time.getTimeStruct());    
    printLog(con.getID(), ": in cache, but expired at " + time_t2str(dead_time));
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
    printLog(c.getID(),
             ": NOTE neither etag nor last-modified specified, resend request");
    return false;
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
  if ((send_len = send(c.getServerFD(), req_new_msg, req_msg_str.size() + 1, 0)) <
      0) {  // send request with validator
    printLog(c.getID(), ":ERROR send validator fails");
    return false;
  }
  char new_resp[65536] = {0};
  int new_len = recv(c.getServerFD(), &new_resp, sizeof(new_resp), 0);
  if (new_len <= 0) {
    printLog(c.getID(), ":ERROR receive validation fails");
    return false;
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
  int post_len = getContentLength(req_msg, len);  //get length of client request
  if (post_len != -1) {
    std::string full_request =
        getFullResponse(connection.getClientFD(), req_msg, len, post_len);
    char send_request[full_request.length() + 1];
    strcpy(send_request, full_request.c_str());
    int slen = send(connection.getServerFD(),
         send_request,
         sizeof(send_request),
         MSG_NOSIGNAL);  // send all the request info from client to server
    if (slen < 0) {
      printLog(connection.getID(), ":ERROR send request to server fails");
      return;
    }
    char response[65536] = {0};
    int response_len = recv(connection.getServerFD(),
                            response,
                            sizeof(response),
                            MSG_WAITALL);  //first time received response from server
    if (response_len >= 0) {
      Response res;
      res.parseStartLine(req_msg, len);
      printLog(connection.getID(),
               ": Received \"" + res.getStartLine() + "\" from " + request.host);

      std::cout << "receive response from server which is:" << response << std::endl;

      if (send(connection.getClientFD(), response, response_len, MSG_NOSIGNAL) > 0) {
        printLog(connection.getID(), ": Responding \"" + res.getStartLine() + "\"");
      } else {
        printLog(connection.getID(), ": ERROR Responding to client fails");
      }      
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
  Response response;
  if (mes_len == 0) {
    try{
      respond502(connection);
    } catch(std::exception & e) {
      printLog(connection.getID(), e.what());
    }
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

    int slen = send(connection.getClientFD(),
         server_msg,
         mes_len,
         0);  //send first response to server
    if (slen < 0) {
      printLog(connection.getID(), ": WARNING send chunk breaks");
      return;
    }
    char chunked_msg[65536] = {0};
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
    printCacheControls(response, connection.getID());           // print cache related note
    int content_len = getContentLength(server_msg, mes_len);  //get content length
    if (content_len != -1) {                           // content_len specified
      std::string msg = getFullResponse(connection.getServerFD(),
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
      int slen = send(connection.getClientFD(), send_msg, msg.length(), 0);
      if (slen < 0) {
        printLog(connection.getID(), ": ERROR send response fails");
        return;
      }
    }
    else {  // content-length not specified, take it as whole message has been received
      int slen = send(connection.getClientFD(), server_msg, mes_len, 0);
      if (slen < 0) {
        printLog(connection.getID(), ": ERROR send response fails");
        return;
      }
    }
    checkAndCache(response, request.start_line, connection.getID());
  }
  printLog(connection.getID(), ": Responding \"" + response.start_line + "\"");
}

void Proxy::printCacheControls(Response & response, int id) {
  if (response.max_age != -1) {
    //C++ 11 only
    printLog(id, ": NOTE Cache-Control: max-age=" + std::to_string(response.max_age));
  }
  if (response.exp_str != "") {
    printLog(id, ": NOTE Expires: " + response.exp_str);
  }
  if (response.no_cache == true) {
    printLog(id, ": NOTE Cache-Control: no-cache");
  }
  if (response.no_store == true) {
    printLog(id, ": NOTE Cache-Control: no-store");
  }
  if (response.must_revalidate == true) {
    printLog(id, ": NOTE Cache-Control: must-revalidate");
  }
  if (response.etag != "") {
    printLog(id, ": NOTE etag: " + response.etag);
  }
  if (response.lastModified != "") {
    printLog(id, ": NOTE Last-Modified: " + response.lastModified);
  }
}

void Proxy::checkAndCache(Response & response, std::string req_start_line, int id) {
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
      printLog(id, ": cached, expires at " + time_t2str(dead_time));
    }
    else if (response.exp_str != "") {
      printLog(id, ": cached, expires at " + response.exp_str);
    }
    else {
      printLog(id,
               ": cached, expired at infinite");
    }
    insertCache(req_start_line, response);
  }
  else {
    printLog(id, ": not cacheable because HTTP/1.1 200 OK not found in response");
  }
}

std::string Proxy::getFullResponse(int send_fd,
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
int Proxy::getContentLength(char * server_msg, int mes_len) {
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
  int slen = send(connection.getClientFD(), "HTTP/1.1 200 OK\r\n\r\n", 19, 0);
  if (slen < 0) {
    printLog(connection.getID(), ": ERROR respond connect fails");
    return;
  }
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

std::string Proxy::time_t2str(time_t tt) {
  struct tm * asc_time = gmtime(&tt);
  const char * t = asctime(asc_time);
  std::string ts(t);
  size_t p = ts.find('\n');
  return ts.substr(0,p);
}

std::string Proxy::getTime() {
  time_t currTime = time(0);
  return time_t2str(currTime);
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
    logFile << "(no-id)" << content_1 << std::endl;
  }
  if (id != -1 && ip == "") {
    logFile << id << content_1 << std::endl;
  }
  if (id != -1 && ip != "") {
    logFile << id << content_1 << ip << content_2 << std::endl;
  }

  mtx_log.unlock();
}

Response Proxy::findCache(const std::string & start_line) {
  mtx_cache.lock();
  std::unordered_map<std::string, Response>::iterator it = cache.begin();
  it = cache.find(start_line);
  if (it == cache.end()) {
    mtx_cache.unlock();
    return Response();
  }else {    
    Response resp(it->second);
    mtx_cache.unlock();
    return resp;
  }
}

void Proxy::insertCache(const std::string & start_line, Response response) {
  mtx_cache.lock();
  if (cache.size() >= 10) {
    std::unordered_map<std::string, Response>::iterator it = cache.begin();    
    printLog(-1, ": NOTE remove " + it->first + " from cache");
    cache.erase(it);
  }
  cache.insert(std::pair<std::string, Response>(start_line, response));
  printLog(-1, ": NOTE insert " + start_line + " into cache");
  printLog(-1, ": NOTE cache size " + std::to_string(cache.size()));
  mtx_cache.unlock();
}

void Proxy::removeCache(const std::string & start_line) {
  mtx_cache.lock();
  cache.erase(start_line);
  printLog(-1, ": NOTE remove " + start_line + " from cache");
  printLog(-1, ": NOTE cache size " + std::to_string(cache.size()));
  mtx_cache.unlock();
}
