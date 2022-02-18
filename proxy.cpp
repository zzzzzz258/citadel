#include "proxy.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "client_info.h"
#include "function.h"
std::mutex mtx;
std::ofstream logFile("proxy.log");
std::unordered_map<std::string, Response> Cache;
void Proxy::run() {
  int temp_fd = build_server(this->port_num);
  if (temp_fd == -1) {
    mtx.lock();
    logFile << "(no-id): ERROR in creating socket to accept" << std::endl;
    mtx.unlock();
    return;
  }
  int client_fd;
  int id = 0;
  while (1) {
    std::string ip;
    client_fd = server_accept(temp_fd, &ip);
    if (client_fd == -1) {
      mtx.lock();
      logFile << "(no-id): ERROR in connecting client" << std::endl;
      mtx.unlock();
      continue;
    }
    mtx.lock();
    Client_Info * client_info = new Client_Info();
    client_info->setFd(client_fd);
    client_info->setIP(ip);
    client_info->setID(id);
    id++;
    mtx.unlock();
    std::thread(handle, client_info).detach();
  }
}

void * Proxy::handle(void * info) {
  Client_Info * client_info = (Client_Info *)info;
  int client_fd = client_info->getFd();

  char req_msg[65536] = {0};
  int len = recv(client_fd, req_msg, sizeof(req_msg), 0);  // fisrt request from client
  if (len <= 0) {
    mtx.lock();
    logFile << client_info->getID() << ": WARNING Invalid Request" << std::endl;
    mtx.unlock();
    return NULL;
  }
  std::string input = std::string(req_msg, len);
  if (input == "" || input == "\r" || input == "\n" || input == "\r\n") {
    return NULL;
  }
  Request * parser = new Request(input);
  if (parser->method != "POST" && parser->method != "GET" &&
      parser->method != "CONNECT") {
    const char * req400 = "HTTP/1.1 400 Bad Request";
    mtx.lock();
    logFile << client_info->getID() << ": Responding \"" << req400 << "\"" << std::endl;
    mtx.unlock();
    return NULL;
  }
  mtx.lock();
  logFile << client_info->getID() << ": \"" << parser->line << "\" from "
          << client_info->getIP() << " @ " << getTime().append("\0");
  mtx.unlock();
  std::cout << "received client request is:" << req_msg << "end" << std ::endl;
  const char * host = parser->host.c_str();
  const char * port = parser->port.c_str();
  std::cout << host << ":" << port << std::endl;
  int server_fd = build_client(host, port);  //connect to server
  if (server_fd == -1) {
    std::cout << "Error in build client!\n";
    return NULL;
  }
  if (parser->method == "CONNECT") {  // handle connect request
    mtx.lock();
    logFile << client_info->getID() << ": "
            << "Requesting \"" << parser->line << "\" from " << host << std::endl;
    mtx.unlock();
    handleConnect(client_fd, server_fd, client_info->getID());
    mtx.lock();
    logFile << client_info->getID() << ": Tunnel closed" << std::endl;
    mtx.unlock();
  }
  else if (parser->method == "GET") {  //handle get request
    int id = client_info->getID();
    bool valid = false;
    std::unordered_map<std::string, Response>::iterator it = Cache.begin();
    it = Cache.find(parser->line);
    if (it == Cache.end()) {  // request not found in cache
      mtx.lock();
      logFile << client_info->getID() << ": not in cache" << std::endl;
      mtx.unlock();
      mtx.lock();
      logFile << client_info->getID() << ": "
              << "Requesting \"" << parser->line << "\" from " << host << std::endl;
      mtx.unlock();
      send(server_fd, req_msg, len, 0);  // send request to server
      handleGet(client_fd, server_fd, client_info->getID(), host, parser->line);
    }
    else {                            //request found in cache
      if (it->second.nocache_flag) {  //has no-cache symbol, revalidate all the time
        if (revalidation(it->second, parser->input, server_fd, id) ==
            false) {  //check Etag and Last Modified
          ask_server(id, parser->line, req_msg, len, client_fd, server_fd, host);
        }
        else {
          use_cache(it->second, id, client_fd);
        }
      }
      else {
        valid =
            CheckTime(server_fd, *parser, parser->line, it->second, client_info->getID());
        if (!valid) {  //ask for server,check res and put in cache if needed
          ask_server(id, parser->line, req_msg, len, client_fd, server_fd, host);
        }
        else {  //send from cache
          use_cache(it->second, id, client_fd);
        }
      }
      printcache();
    }
  }
  else if (parser->method == "POST") {  //handle post request
    mtx.lock();
    logFile << client_info->getID() << ": "
            << "Requesting \"" << parser->line << "\" from " << host << std::endl;
    mtx.unlock();
    handlePOST(client_fd, server_fd, req_msg, len, client_info->getID(), host);
  }
  close(server_fd);
  close(client_fd);
  return NULL;
}

void Proxy::ask_server(int id,
                       std::string line,
                       char * req_msg,
                       int len,
                       int client_fd,
                       int server_fd,
                       const char * host) {
  mtx.lock();
  logFile << id << ": "
          << "Requesting \"" << line << "\" from " << host << std::endl;
  mtx.unlock();

  send(server_fd, req_msg, len, 0);
  handleGet(client_fd, server_fd, id, host, line);
}
void Proxy::use_cache(Response & res, int id, int client_fd) {
  char cache_res[res.getSize()];
  strcpy(cache_res, res.getResponse());
  send(client_fd, cache_res, res.getSize(), 0);
  mtx.lock();
  logFile << id << ": Responding \"" << res.line << "\"" << std::endl;
  mtx.unlock();
}
void Proxy::printcache() {
  std::unordered_map<std::string, Response>::iterator it = Cache.begin();
  std::cout << "****************Cache****************-" << std::endl;
  while (it != Cache.end()) {
    std::cout << "---------------Check Begin---------------" << std::endl;
    std::cout << "req_line ====== " << it->first << std::endl;
    std::string header = it->second.response.substr(0, 300);
    std::cout << "response header in cache =======" << header << std::endl;
    std::cout << "---------------end of header---------------" << std::endl;
    ++it;
  }
  std::cout << "****************Size*************-" << std::endl;
  std::cout << "cache.size=" << Cache.size() << std::endl;
  std::cout << "****************Cache end*************-" << std::endl;
}
bool Proxy::CheckTime(int server_fd,
                      Request & parser,
                      std::string req_line,
                      Response & rep,
                      int id) {
  if (rep.max_age != -1) {
    time_t curr_time = time(0);
    time_t rep_time = mktime(rep.response_time.getTimeStruct());
    int max_age = rep.max_age;
    if (rep_time + max_age <= curr_time) {
      Cache.erase(req_line);
      time_t dead_time = mktime(rep.response_time.getTimeStruct()) + rep.max_age;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      mtx.lock();
      logFile << id << ": in cache, but expired at " << t;
      mtx.unlock();
      return false;
    }
  }

  if (rep.exp_str != "") {
    time_t curr_time = time(0);
    time_t expire_time = mktime(rep.expire_time.getTimeStruct());
    if (curr_time > expire_time) {
      Cache.erase(req_line);
      time_t dead_time = mktime(rep.expire_time.getTimeStruct());
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      mtx.lock();
      logFile << id << ": in cache, but expired at " << t;
      mtx.unlock();
      return false;
    }
  }
  bool revalid = revalidation(rep, parser.input, server_fd, id);
  if (revalid == false) {
    return false;
  }
  mtx.lock();
  logFile << id << ": in cache, valid" << std::endl;
  mtx.unlock();
  return true;
}
/**
 * A function to check if revalidation is necessary
 * @return: true if no need to revalidate, false means revalidation is needed
 */
bool Proxy::revalidation(Response & rep, std::string input, int server_fd, int id) {
  if (rep.ETag == "" && rep.LastModified == "") {  // no validator available
    return true;
  }
  std::string changed_input = input;
  if (rep.ETag != "") {
    std::string add_etag = "If-None-Match: " + rep.ETag.append("\r\n");
    changed_input = changed_input.insert(changed_input.length() - 2, add_etag);
  }
  if (rep.LastModified != "") {
    std::string add_modified = "If-Modified-Since: " + rep.LastModified.append("\r\n");
    changed_input = changed_input.insert(changed_input.length() - 2, add_modified);
  }
  std::string req_msg_str = changed_input;
  //  char req_new_msg[req_msg_str.size() + 1];
  const char * req_new_msg = changed_input.c_str();
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
    mtx.lock();
    logFile << id << ": in cache, requires validation" << std::endl;
    mtx.unlock();
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
      res.ParseLine(req_msg, len);
      mtx.lock();
      logFile << id << ": Received \"" << res.getLine() << "\" from " << host
              << std::endl;
      mtx.unlock();

      std::cout << "receive response from server which is:" << response << std::endl;

      send(client_fd, response, response_len, MSG_NOSIGNAL);

      mtx.lock();
      logFile << id << ": Responding \"" << res.getLine() << std::endl;
      mtx.unlock();
    }
    else {
      std::cout << "server socket closed!\n";
    }
  }
}
void Proxy::handleGet(int client_fd,
                      int server_fd,
                      int id,
                      const char * host,
                      std::string req_line) {
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
  Response parse_res;
  parse_res.ParseLine(server_msg, mes_len);  // parse and get the first line
  parse_res.response = std::string(server_msg, mes_len);

  mtx.lock();
  logFile << id << ": Received \"" << parse_res.getLine() << "\" from " << host
          << std::endl;
  mtx.unlock();

  bool is_chunk = findChunk(server_msg, mes_len);
  if (is_chunk) {  // chunked response, no cache, just resend
    mtx.lock();
    logFile << id << ": not cacheable because it is chunked" << std::endl;
    mtx.unlock();

    send(client_fd, server_msg, mes_len, 0);  //send first response to server
    char chunked_msg[28000] = {0};
    while (1) {  //receive and send remaining message
      int len = recv(server_fd, chunked_msg, sizeof(chunked_msg), 0);
      if (len <= 0) {
        std::cout << "chunked break\n";
        break;
      }
      send(client_fd, chunked_msg, len, 0);
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
    parse_res.ParseField(server_msg, mes_len);         // fill attributes of response
    printnote(parse_res, id);                          // print cache related note
    int content_len = getLength(server_msg, mes_len);  //get content length
    if (content_len != -1) {                           // content_len specified
      std::string msg = sendContentLen(
          server_fd, server_msg, mes_len, content_len);  //get the entire message
      parse_res.response = msg;
      // send response to client
      std::vector<char> large_msg;
      for (size_t i = 0; i < msg.length(); i++) {
        large_msg.push_back(msg[i]);
      }
      const char * send_msg = large_msg.data();
      send(client_fd, send_msg, msg.length(), 0);
    }
    else {  // content-length not specified, take it as whole message has been received
      std::string server_msg_str(server_msg, mes_len);
      parse_res.setEntireRes(server_msg_str);
      send(client_fd, server_msg, mes_len, 0);
    }
    printcachelog(parse_res, no_store, req_line, id);
  }
  // print messages
  std::cout << "Responding for GET\n";
  std::string logrespond(server_msg, mes_len);
  size_t log_pos = logrespond.find_first_of("\r\n");
  std::string log_line = logrespond.substr(0, log_pos);
  mtx.lock();
  std::cout << "logfile responding\n";
  logFile << id << ": Responding \"" << log_line << "\"" << std::endl;
  mtx.unlock();
}

void Proxy::Check502(std::string entire_msg, int client_fd, int id) {
  if (entire_msg.find("\r\n\r\n") == std::string::npos) {
    const char * bad502 = "HTTP/1.1 502 Bad Gateway";
    send(client_fd, bad502, sizeof(bad502), 0);
    mtx.lock();
    logFile << id << ": Responding \"HTTP/1.1 502 Bad Gateway\"" << std::endl;
    mtx.unlock();
  }
}

void Proxy::printnote(Response & parse_res, int id) {
  if (parse_res.max_age != -1) {
    mtx.lock();
    logFile << id << ": NOTE Cache-Control: max-age=" << parse_res.max_age << std::endl;
    mtx.unlock();
  }
  if (parse_res.exp_str != "") {
    mtx.lock();
    logFile << id << ": NOTE Expires: " << parse_res.exp_str << std::endl;
    mtx.unlock();
  }
  if (parse_res.nocache_flag == true) {
    mtx.lock();
    logFile << id << ": NOTE Cache-Control: no-cache" << std::endl;
    mtx.unlock();
  }
  if (parse_res.ETag != "") {
    mtx.lock();
    logFile << id << ": NOTE ETag: " << parse_res.ETag << std::endl;
    mtx.unlock();
  }
  if (parse_res.LastModified != "") {
    mtx.lock();
    logFile << id << ": NOTE Last-Modified: " << parse_res.LastModified << std::endl;
    mtx.unlock();
  }
}
void Proxy::printcachelog(Response & parse_res,
                          bool no_store,
                          std::string req_line,
                          int id) {
  mtx.lock();
  logFile << id << ": function printachelog called " << std::endl;
  mtx.unlock();
  if (parse_res.response.find("HTTP/1.1 200 OK") != std::string::npos) { // cacheable response
    if (no_store) {  // no-store specified
      mtx.lock();
      logFile << id << ": not cacheable becaues NO STORE" << std::endl;
      mtx.unlock();
      return;
    }
    if (parse_res.max_age != -1) {  // max-age specified
      time_t dead_time =
          mktime(parse_res.response_time.getTimeStruct()) + parse_res.max_age;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      mtx.lock();
      logFile << id << ": cached, expires at " << t << std::endl;
      mtx.unlock();
    }
    else if (parse_res.exp_str != "") {
      mtx.lock();
      logFile << id << ": cached, expires at " << parse_res.exp_str << std::endl;
      mtx.unlock();
    }
    // not dealing with the situation that neither max-age nor expired-time is sprcified
    Response storedres(parse_res);
    if (Cache.size() > 10) {
      std::unordered_map<std::string, Response>::iterator it = Cache.begin();
      Cache.erase(it);
    }
    Cache.insert(std::pair<std::string, Response>(req_line, storedres));
    mtx.lock();
    logFile << id << ": ADD NEW item to cache" << std::endl;
    mtx.unlock();
  }
  mtx.lock();
  logFile << id << ": HTTP/1.1 200 OK not found in response, not cache it" << parse_res.LastModified << std::endl;
  mtx.unlock();
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
  mtx.lock();
  logFile << id << ": Responding \"HTTP/1.1 200 OK\"" << std::endl;
  mtx.unlock();
  fd_set readfds;
  int nfds = server_fd > client_fd ? server_fd + 1 : client_fd + 1;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    FD_SET(client_fd, &readfds);

    select(nfds, &readfds, NULL, NULL, NULL);
    int fd[2] = {server_fd, client_fd};
    int len;
    for (int i = 0; i < 2; i++) {
      char message[65536] = {0};
      if (FD_ISSET(fd[i], &readfds)) {
        len = recv(fd[i], message, sizeof(message), 0);
        if (len <= 0) {
          return;
        }
        else {
          if (send(fd[1 - i], message, len, 0) <= 0) {
            return;
          }
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
