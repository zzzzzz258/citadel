#include "response.h"

const std::vector<char> & Response::getRawContent() {
  return raw_content;
}

const std::string Response::getRawContentString(int size) {
  if (size > getSize()) {
    size = getSize();
  }
  std::string ans;
  for (int i = 0; i < size; i++) {
    ans.push_back(raw_content[i]);
  }
  return ans;
}

void Response::setRawContent(const std::vector<char> & msg) {
  raw_content = msg;
}

void Response::setRawContent(const std::string & msg) {
  raw_content.clear();
  for (auto it = msg.begin(); it != msg.end(); it++) {
    raw_content.push_back(*it);
  }
}

int Response::getSize() {
  return raw_content.size();
}
void Response::parseStartLine(char * first_part, int len) {
  std::string first_part_str(first_part, len);
  size_t pos = first_part_str.find_first_of("\r\n");
  start_line = first_part_str.substr(0, pos);
}

void Response::parseField(char * first_msg, int len) {
  std::string msg(first_msg, len);
  size_t date_pos;
  if ((date_pos = msg.find("Date: ")) != std::string::npos) {
    size_t GMT_pos = msg.find(" GMT");
    std::string date_str = msg.substr(date_pos + 6, GMT_pos - date_pos - 6);
    response_time.init(date_str);
  }
  size_t max_age_pos;
  if ((max_age_pos = msg.find("max-age=")) != std::string::npos) {
    size_t max_age_end_pos = msg.find("\r\n", max_age_pos+9);
    std::string max_age_str = msg.substr(max_age_pos+9, max_age_end_pos);
    max_age = atoi(max_age_str.c_str());
    std::cout << "\n\nmax-age: " << max_age_str << ", int: " << max_age << std::endl;
  }
  size_t expire_pos;
  if ((expire_pos = msg.find("Expires: ")) != std::string::npos) {
    size_t GMT_pos = msg.find(" GMT");
    exp_str = msg.substr(expire_pos + 9, GMT_pos - expire_pos - 9);
    expire_time.init(exp_str);
  }
  size_t nocatch_pos;
  if ((nocatch_pos = msg.find("no-cache")) != std::string::npos) {
    no_cache = true;
  }
  size_t mustrevalidate_pos;
  if ((mustrevalidate_pos = msg.find("must-revalidate")) != std::string::npos) {
    no_cache = true;
  }

  size_t etag_pos;
  if ((etag_pos = msg.find("ETag: ")) != std::string::npos) {
    size_t etag_end = msg.find("\r\n", etag_pos + 6);
    etag = msg.substr(etag_pos + 6, etag_end - etag_pos - 6);
  }
  size_t lastmodified_pos;
  if ((lastmodified_pos = msg.find("Last-Modified: ")) != std::string::npos) {
    size_t lastmodified_end = msg.find("\r\n", lastmodified_pos + 15);
    lastModified =
        msg.substr(lastmodified_pos + 15, lastmodified_end - lastmodified_pos - 15);
  }
}
