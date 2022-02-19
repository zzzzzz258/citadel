#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
class TimeMap {
 public:
  std::map<std::string, int> time_map;

  TimeMap() { init(); }
  void init();
  int get(std::string str) { return time_map.find(str)->second; }
};

class MyTime {
 private:
  struct tm mytime;

 public:
  MyTime() {}
  void init(std::string exp);
  struct tm * getTimeStruct() {
    return &mytime;
  }
};
