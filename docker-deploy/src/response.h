#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "mytime.h"
class Response {
 public:
  std::vector<char> raw_content;
  std::string start_line;
  int max_age;
  MyTime expire_time;
  std::string exp_str;
  MyTime response_time;
  bool chunked;
  bool no_store;
  bool no_cache;
  bool must_revalidate;
  // two validators
  std::string etag;
  std::string lastModified;

 public:
  Response() :
      max_age(-1),
      exp_str(""),
      chunked(false),
      no_store(false),
      no_cache(false),
      must_revalidate(false),
      etag(""),
      lastModified("") {}
  std::string getStartLine() { return start_line; }
  void parseStartLine(const char * first_part, int len);  // parse and fill start line
  int getSize();                                          // get raw content size
  const std::vector<char> & getRawContent();
  const std::string getRawContentString(int size);
  void setRawContent(const std::string & msg);
  void setRawContent(const std::vector<char> & msg);
  void parseField(char * first_msg, int len);
  void parseChunk();
  void parseNoStore();
};
