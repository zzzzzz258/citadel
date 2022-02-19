all: main

main: main.cpp function.cpp proxy.cpp proxy.h request.cpp response.cpp mytime.cpp
	g++ -g  -o main main.cpp function.cpp proxy.cpp request.cpp response.cpp mytime.cpp -lpthread

.PHONY:
	clean
clean:
	rm -rf *.o main
