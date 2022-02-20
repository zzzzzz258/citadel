#include <stdio.h>
#include <string.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
class Request {
 public:
  std::string raw_content;
  std::string host;
  std::string port;
  std::string method;
  std::string start_line;
  bool no_cache;

 public:
  Request(std::string request) : raw_content(request) {
    //    std::cout << "request origin: " << request << std::endl;
    //std::cout<<"request in parse function is"<<raw_content<<std::endl;
    parseRawContent();
    parseMethod();
    parseStartLine();
    parseNoCache();
  }
  void parseStartLine();
  void parseRawContent();
  void parseMethod();
  void parseNoCache();
  bool solvable();
};
