CXX = g++
CXXFLAGS=-Wall -Wextra -g -O1 -std=c++17

SRC_FILES = jukebox-server.cpp ChunkedDataSender.cpp ConnectedClient.cpp
TARGETS = jukebox-server

all: $(TARGETS)

jukebox-server: $(SRC_FILES) ChunkedDataSender.h ConnectedClient.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_FILES)

clean:
	rm -f $(TARGETS)
