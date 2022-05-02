/*
 * File: jukebox-server.cpp
 *
 * Authors: Matt Willis and Kevin Mcdonald
 * authors
 * Server for an Internet Jukebox.
 */

// C++ standard libraries
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <filesystem>

// C standard libraries
#include <cerrno>

// POSIX and OS-specific libraries
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "ChunkedDataSender.h"
#include "ConnectedClient.h"

namespace fs = std::filesystem;

using std::cout;
using std::cerr;
using std::string;
using std::vector;
using std::map;

const int BACKLOG = 10;
const int MAX_EVENTS = 64;

// forward declarations
int accept_connection(int server_socket);
int setup_server_socket(uint16_t port_num);
void set_non_blocking(int sock);
int find_mp3_files(const char *dir);
void event_loop(int epoll_fd, int server_socket);

int main(int argc, char **argv) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <port> <filedir>\n";
        exit(EXIT_FAILURE);
    }

	if (!fs::is_directory(argv[2])) {
		cerr << "ERROR: " << argv[2] << " is not a directory\n";
		exit(EXIT_FAILURE);
	}

    // Get the port number from the arguments.
    uint16_t port = (uint16_t) std::stoul(argv[1]);

	int serv_sock = setup_server_socket(port);

    /* 
	 * Read the other argument (mp3 directory).
	 * See the notes for this function above.
	 */
    int song_count = find_mp3_files(argv[2]);
    cout << "Found " << song_count << " songs.\n";

	// Create the epoll, which returns a file descriptor for us to use later.
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	// We want to watch for input events (i.e. connection requests) on our
	// server socket.
	struct epoll_event server_ev;
	server_ev.data.fd = serv_sock;
	server_ev.events = EPOLLIN;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serv_sock, &server_ev) == -1) {
		perror("epoll_ctl");
		exit(EXIT_FAILURE);
	}

	event_loop(epoll_fd, serv_sock);
}

/**
 * Creates a socket, sets it to non-blocking, binds it to the given port, then
 * sets it to start listen for incoming connections.
 *
 * @param port_num The port number we will listen on.
 * @return The file descriptor of the newly created/setup server socket.
 */
int setup_server_socket(uint16_t port_num) {
    /* Create the socket that we'll listen on. */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    /* Set SO_REUSEADDR so that we don't waste time in TIME_WAIT. */
    int val = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, 
							&val, sizeof(val));
    if (val < 0) {
        perror("Setting socket option failed");
        exit(EXIT_FAILURE);
    }

    /* 
	 * Set our server socket to non-blocking mode.  This way, if we
     * accidentally accept() when we shouldn't have, we won't block
     * indefinitely.
	 */
    set_non_blocking(sock_fd);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    addr.sin_addr.s_addr = INADDR_ANY;

    /* Bind our socket and start listening for connections. */
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Error binding to port");
        exit(EXIT_FAILURE);
    }

    if (listen(sock_fd, BACKLOG) < 0) {
        perror("Error listening for connections");
        exit(EXIT_FAILURE);
    }

	return sock_fd;
}

/**
 * Accepts a connection and returns the socket descriptor of the new client
 * that has connected to us.
 *
 * @param server_socket Socket descriptor of the server (that is listening)
 * @return Socket descriptor for newly connected client.
 */
int accept_connection(int server_socket) {
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof(their_addr);
	int new_fd = accept(server_socket, (struct sockaddr *)&their_addr,
						&addr_size);
	if (new_fd < 0) {
		perror("accept");
		exit(EXIT_FAILURE);
	}

	return new_fd;
}

/* 
 * Use fcntl (file control) to set the given socket to non-blocking mode.
 * With non-blocking mode set, any time you try to call send or recv that
 * would normally block, it will instead immediately return -1 and set errno
 * to EAGAIN (or EWOULDBLOCK).
 *
 * @param sock The file descriptor for the socket you want to make
 * 				non-blocking.
 */
void set_non_blocking(int sock) {
    // Get the current flags
    int socket_flags = fcntl(sock, F_GETFL);
    if (socket_flags < 0) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

	// Add in the nonblock option
    socket_flags = socket_flags | O_NONBLOCK;

    // Set the new flags, including O_NONBLOCK.
    int result = fcntl(sock, F_SETFL, socket_flags);
    if (result < 0) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }
}


/*
 * Given a path to a directory, this function searches the directory for any
 * files that end in ".mp3".
 * When it files an MP3 file, it also looks for an associated ".info" file, and
 * prints the contents of this file if it exists.
 *
 * @info You'll need to edit this to meet your needs (i.e. don't expect this
 * to do everything you want without any effort). I would recommend making it
 * return a vector (C++'s version of Java's ArrayList) containing the paths to
 * each of the mp3 files you find.
 *
 * @param dir String that represents the path to the directory that you want
 * 				to check.
 *
 * @return Number of MP3 files found inside of the specified directory.
 */
int find_mp3_files(const char *dir) {
	int num_mp3_files = 0;

	// Loop through all files in the directory
	for(fs::directory_iterator entry(dir); entry != fs::directory_iterator(); ++entry) {
		string filename = entry->path().filename().string();

		// See if the current file is an MP3 file
		if (entry->path().extension() == ".mp3") {
			cout << "(" << num_mp3_files << ") " << filename << "\n";
			num_mp3_files++;

			// Look for an associated info file
			fs::path info_file_path = entry->path();
			info_file_path = info_file_path.replace_extension(".mp3.info");
			if (fs::is_regular_file(info_file_path)) {
				// read contents of file into a buffer
				std::ifstream t(info_file_path.string());
				std::stringstream buffer;
				buffer << t.rdbuf();
				cout << "Info:\n" << buffer.str() << "\n";
			}
		}
	}

    return num_mp3_files;
}

/**
 * Accepts a new client then sets the server up to be ready to receive data
 * from that client.
 * After exiting, we'll have a new client set to RECEIVING mode, our socket to
 * that client will be non-blocking, and our epoll interest list will contain
 * this new client (watching for inputs or closes from the client).
 *
 * @param server_socket Socket listening for new connections.
 * @param clients Mapping between client sockets and client info
 * @param epoll_fd File descriptor for epoll
 */
void setup_new_client(int server_socket, 
						map<int, ConnectedClient> &clients, 
						int epoll_fd) {
	int client_fd = accept_connection(server_socket);
	cout << "Accepted a new connection!\n";

	// The client_fd shouldn't exist in our clients map.
	if (clients.find(client_fd) != clients.end()) {
		cerr << "ERROR: File descriptor already mapped to an existing client.\n";
		exit(EXIT_FAILURE);
	}

	// Set this to non-blocking mode so we never get hung up
	// trying to send or receive from this client.
	set_non_blocking(client_fd);

	// Watch for "input" and "hangup" events for new clients.
	struct epoll_event new_client_ev;
	new_client_ev.events = EPOLLIN | EPOLLRDHUP;
	new_client_ev.data.fd = client_fd;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, 
					&new_client_ev) == -1) {
		perror("epoll_ctl: client_fd");
		exit(EXIT_FAILURE);
	}

	// We have a new client so we'll create a new ConnectClient object to
	// represent this new client.
	ConnectedClient cc(client_fd, RECEIVING);

	// Add this new connected client to our map from file descriptor to client.
	clients[client_fd] = cc;
}

/**
 * Waits for epoll events then handles them accordingly.
 *
 * @param epoll_fd File descriptor for our epoll.
 * @param server_socket Socket that is listening for connections.
 */
void event_loop(int epoll_fd, int server_socket) {
	// associate client's file descriptor with its ConnectedClient object
	map<int, ConnectedClient> clients;

    while (true) {
		// wait for some events to occur, writing them to our events array
		struct epoll_event events[MAX_EVENTS];

		int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (num_events < 0) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		// Loop through all the I/O events that just happened.
		for (int n = 0; n < num_events; n++) {
			// Check if this is a "hang up" event (i.e. client closed the
			// connection).
			if ((events[n].events & EPOLLRDHUP) != 0) {
				// If we get here, the socket associated with this event was
				// closed by the remote host so we should clean up.
				clients[events[n].data.fd].handle_close(epoll_fd);
				clients.erase(events[n].data.fd);
			}

			// Check if this is an "input" event (i.e. ready to "read" from
			// this socket)
			else if ((events[n].events & EPOLLIN) != 0) {
				if (events[n].data.fd == server_socket) {
					/*
					 * If the server socket is ready for "reading," that implies
					 * we have a new client that wants to connect so lets
					 * set up that new client now.
					 */
					setup_new_client(server_socket, clients, epoll_fd);
				}
				else {
					/*
					 * This wasn't the server socket so this means we have a
					 * client that has sent us data so we can receive it now
					 * without worrying about blocking.
					 */
					clients[events[n].data.fd].handle_input(epoll_fd);
				}
            }

			// Check if this is an "output" event.
			// Note: You may want/need to make this an else if, depending on
			// how you are handling clients.
			if ((events[n].events & EPOLLOUT) != 0) {
				/* 
				 * If you set things up correctly, you should only reach this
				 * point if you started sending a response, but had to stop.
				 * You'll therefore need to continue sending whatever response
				 * you had in progress.
				 */
				// TODO: Create a new function in your ConnectedClient class
				// and call that here, sort of like what was done for
				// handle_input and handle_close earlier in this function.
            }
        }
    }
}
