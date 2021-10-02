CXXSOURCES = server.cpp 
CXX = g++
CXXFLAGS = -std=c++17 -lstdc++fs

all: server

server: 
	$(CXX) $(CXXSOURCES) $(CXXFLAGS) -o serwer

.PHONY: clean
clean:
	rm -rf *.o serwer