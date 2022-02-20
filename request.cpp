#include "request.h"

#include <cstring>
#include <exception>
#include <iostream>
void Request::parseStartLine() {
  size_t pos = raw_content.find_first_of("\r\n");
  start_line = raw_content.substr(0, pos);
}
void Request::parseMethod() {
  size_t method_end = raw_content.find_first_of(" ");
  method = raw_content.substr(0, method_end);
}

void Request::parseRawContent() {
  try {
    size_t pos = raw_content.find("Host: ");
    std::string after_host = raw_content.substr(pos + 6);
    size_t host_line_end;
    host_line_end = after_host.find_first_of("\r\n");
    std::string host_line = after_host.substr(0, host_line_end);
    size_t port_begin;
    if ((port_begin = host_line.find_first_of(":\r")) != std::string::npos) {
      host = after_host.substr(0, port_begin);
      port = host_line.substr(port_begin + 1);
    }
    else {
      host = host_line;
      port = "80";
    }
  }
  catch (std::exception & e) {
    std::cout << "Host is Empty!" << std::endl;
    host = "";
    port = "";
    return;
  }
}

void Request::parseNoCache() {
  size_t nocatch_pos;
  if ((nocatch_pos = raw_content.find("no-cache")) != std::string::npos) {
    no_cache = true;
  }
  else {
    no_cache = false;
  }
}

bool Request::solvable() {
  if (method == "POST" || method == "GET" || method == "CONNECT") {
    return true;
  }
  return false;
}
