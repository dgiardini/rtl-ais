// ------------------------------------------------------------
// tcp_listener.c
// Written by Peter Schultz, hp@hpes.dk
// ------------------------------------------------------------
#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/select.h>

#if defined (__WIN32__)
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <netdb.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
    #include <arpa/inet.h>
#endif

typedef struct t_sockIo {
	int sock;
	pthread_t thread_t;
	int sesion_active;
	struct sockaddr_in cli_addr;
	char from_ip[20];
	struct t_sockIo *next;

	// An active listener thread has a `pipe()` allocated, whose file
	// descriptors are stored here. This pipe is written to with new
	// AIS messages as they are decoded if `_tcp_stream_forever` is set.
	int msgpipe[2];

} TCP_SOCK, *P_TCP_SOCK;

static int sockfd;
static int _debug_nmea = 0;
static int _debug = 0;
static int _tcp_keep_ais_time = 15;
static int _tcp_stream_forever = 0;
static int portno;
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;;

// Linked list vars.
P_TCP_SOCK head = (P_TCP_SOCK) NULL;
P_TCP_SOCK end = (P_TCP_SOCK) NULL;

pthread_t tcp_listener_thread;

typedef struct t_ais_mess {
	char message[100];  // max on nmea message is 83 char's
	char *plmess;
	int length;
	struct timeval timestamp;
	struct t_ais_mess *next;
} AIS_MESS, *P_AIS_MESS;

// Linked list ais messages.
P_AIS_MESS ais_head = (P_AIS_MESS) NULL;
P_AIS_MESS ais_end = (P_AIS_MESS) NULL;

pthread_mutex_t ais_lock=PTHREAD_MUTEX_INITIALIZER;;

// Local Prototypes
P_TCP_SOCK init_node();
void add_node(P_TCP_SOCK new_node);
void delete_node(P_TCP_SOCK p);
int accept_c(P_TCP_SOCK p_tcp_sock);
int error_category(int rc);
static void *tcp_listener_fn(void *arg);
void *handle_remote_close(void *arg);
void delete_ais_node(P_AIS_MESS p);
void remove_old_ais_messages( );

#include "tcp_listener.h"

int initTcpSocket(const char *portnumber, int debug_nmea, int tcp_keep_ais_time, int tcp_stream_forever) {
	_debug_nmea = debug_nmea;
	_tcp_keep_ais_time = tcp_keep_ais_time;
	_tcp_stream_forever = tcp_stream_forever;
	struct sockaddr_in serv_addr;
#if defined (__WIN32__)
	WSADATA wsaData;
	WORD wVersionRequested;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
#endif
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "Failed to create socket! error %d\n", errno);
		return 0;
	}
	memset((char *) &serv_addr, 0, sizeof(serv_addr));
	portno = atoi(portnumber);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
		fprintf(stderr, "setsockopt(SO_REUSEADDR) failed! error %d\n", errno);
		return 0;
	}

	if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		fprintf(stderr, "Failed to bind socket! error %d\n", errno);
		return 0;
	}

	if (listen(sockfd, MAX_TCP_CONNECTIONS) < 0) {
		fprintf(stderr, "listen failed with error %d\n", errno);
		return 0;
	}

	pthread_create(&tcp_listener_thread, NULL, tcp_listener_fn, (void *) NULL);

	return 1;
}

void closeTcpSocket() {

	// wait for socket shutdown complete
	shutdown( sockfd,2);
#if defined (__WIN32__)
	Sleep(3000);
	closesocket(sockfd);
#else
	sleep(3);
	close(sockfd);
#endif
}

// ------------------------------------------------------------
// The main listener loop thread
// ------------------------------------------------------------
static void *tcp_listener_fn(void *arg) {
	int rc;
	P_TCP_SOCK t;
	(void)(arg); // not used, avoid compiling warnings

	fprintf(stderr, "Tcp listen port %d\nAis message timeout with %d\n", portno, _tcp_keep_ais_time);

	while (1) {

		t = init_node();

		rc = accept_c(t);

		if ( rc == -1)
			break;

		if (rc == -2) {
#if defined (__WIN32__)
			closesocket(t->sock);
#else		
			close(t->sock);
#endif		
			free(t);
			continue;
		}
		add_node(t);
		pthread_create(&t->thread_t, NULL, handle_remote_close, (void *) t);

	}
		shutdown( sockfd,2);
#if defined (__WIN32__)
		closesocket(t->sock);
#else		
		close(t->sock);
#endif		
	return 0;
	
	
}

// ------------------------------------------------------------
// thread func for handling a remote TCP client
// ------------------------------------------------------------
void *handle_remote_close(void *arg) {
	P_TCP_SOCK t = (P_TCP_SOCK) arg;
	P_AIS_MESS ais_temp;

	// On connect, send all saved ais_messages to new socket
	pthread_mutex_lock(&ais_lock);
	ais_temp = ais_head;
	while (ais_temp != NULL) {
		int rc;
		if (ais_temp->plmess != NULL)
			rc = send(t->sock, ais_temp->plmess, ais_temp->length, 0);
		else
			rc = send(t->sock, ais_temp->message, ais_temp->length, 0);
		if( _debug)
			fprintf( stdout, "%ld: Send to %s, <%.*s>, rc=%d\n",ais_temp->timestamp.tv_sec, t->from_ip, ais_temp->length, ais_temp->message, rc);
		ais_temp = ais_temp->next;
	}
	pthread_mutex_unlock(&ais_lock);

	while (1) {
		fd_set fds;
		int res;
		int msgfd = t->msgpipe[0];
		const int nfds = (t->sock > msgfd ? t->sock : msgfd) + 1;

		// Listen on the client socket, and on the internal pipe which receives
		// new messages. (New messages will only be sent to this pipe when the
		// program is launched with the '-k' flag.)
		FD_ZERO(&fds);
		FD_SET(t->sock, &fds);
		FD_SET(msgfd, &fds);
		
		res = pselect(nfds, &fds, NULL, NULL, NULL, NULL);
		if (res < 0) {
			if (_debug)
				perror("select error");
			break;
		}

		// Service remote client socket: If the client sends any data, close the
		// socket (legacy behavior).
		if (FD_ISSET(t->sock, &fds)) {
			unsigned char buff[100];
			int rc;
			rc = recv(t->sock, (char *)buff, 99, 0);
			if (rc < 0) {
				if (_debug && errno != EAGAIN)
					perror("socket error");
				break;
			} else if (rc >= 0) {
				if (_debug)
					fprintf(stdout, "client closed the socket\n");
				break;
			}
		}

		// If new messages became available internally, send them to the client.
		if (FD_ISSET(msgfd, &fds)) {
			unsigned char buff[1024];
			int rc;
			rc = read(msgfd, (char *)buff, 1024);
			if (rc < 0) {
				if (_debug) {
					perror("pipe error");
				}
				break;
			} else if (rc == 0) {
				if (_debug) {
					fprintf(stderr, "pipe closed\n");
				}
				break;
			}

			rc = send(t->sock, buff, rc, 0);
			if (rc < 0) {
				if (_debug)
					perror("error writing to client");
				break;
			}
		}
	}
	shutdown(t->sock, 2);
#if defined (__WIN32__)
		closesocket(t->sock);
#else		
		close(t->sock);
#endif		
	delete_node(t);
	return 0;
}



// ------------------------------------------------------------
// Accept call
// ------------------------------------------------------------
int accept_c(P_TCP_SOCK p_tcp_sock) {

	int optval = 1; // Keep alive
	socklen_t optlen = sizeof(optval);
	socklen_t clilen = sizeof(p_tcp_sock->cli_addr);

	/* wait for connection on local port.*/
	if ((p_tcp_sock->sock = accept(sockfd, (struct sockaddr*) &p_tcp_sock->cli_addr, &clilen)) < 0) {
		fprintf(stderr, "Failed to accept socket!, error = %d\n", errno);
		if( errno == 22) return -1;
		return error_category(errno);
	}

	if (setsockopt(p_tcp_sock->sock, SOL_SOCKET, SO_KEEPALIVE,(char *) &optval, optlen) < 0) {
		fprintf(stderr, "Failed to set option keepalive!, error = %d\n", errno);
		return error_category(errno);
	}

	sprintf(p_tcp_sock->from_ip, "%.*s", 19, inet_ntoa(p_tcp_sock->cli_addr.sin_addr));
	if (_debug) {
		fprintf(stdout, "connect from %s\n", p_tcp_sock->from_ip);
	}

	p_tcp_sock->sesion_active = 1;

	return 0;
}

// ------------------------------------------------------------
// Remove messages older than timeout
// ------------------------------------------------------------
void remove_old_ais_messages( ) {
	struct timeval now;
	P_AIS_MESS temp_1;
	P_AIS_MESS temp;
	gettimeofday(&now, NULL);

	temp = ais_head;

	while (temp != NULL) {
		if ((int) (now.tv_sec - temp->timestamp.tv_sec) > _tcp_keep_ais_time) {
			if( _debug)
				fprintf(stdout, "remove mess <%.*s>, timeout %ld\n", temp->length, temp->message, (long) (now.tv_sec - temp->timestamp.tv_sec));
			temp_1 = temp->next;
			pthread_mutex_lock(&ais_lock);
			delete_ais_node(temp);
			pthread_mutex_unlock(&ais_lock);
			temp = temp_1;
		} else {
			temp = temp->next;
		}
	}
}

// ------------------------------------------------------------
// send ais message to all clients
// ------------------------------------------------------------
int add_nmea_ais_message(const char * mess, unsigned int length) {
	P_AIS_MESS new_node;

	// remove eventually old messages
	remove_old_ais_messages();

	pthread_mutex_lock(&ais_lock);

	// allocate an add the new message
	new_node = (P_AIS_MESS) malloc(sizeof(AIS_MESS));
	if (length>=sizeof(new_node->message)) {
		new_node->plmess = malloc(length);
		if(new_node->plmess == NULL) {
			free(new_node);
			return -1;
		} 
		strncpy(new_node->plmess, mess, length);
		new_node->message[0] = 0; // Just in case
	} else {
		new_node->plmess = NULL;
		strncpy(new_node->message, mess, length);
	}
	new_node->length = length;
	gettimeofday(&new_node->timestamp, NULL);

	if (ais_head == NULL) {
		ais_head = new_node;
		ais_end = new_node;
	}
	ais_end->next = new_node;
	new_node->next = NULL;
	ais_end = new_node;

	pthread_mutex_unlock(&ais_lock);

	// Send the message to all active clients.
	if (_tcp_stream_forever) {
		P_TCP_SOCK tcp_client;

		pthread_mutex_lock(&lock);
		tcp_client = head;
		while (tcp_client != NULL) {
			int retval;
			retval = write(tcp_client->msgpipe[1], mess, length);
			tcp_client = tcp_client->next;
		}
		pthread_mutex_unlock(&lock);
	}


	return 0;
}

// ------------------------------------------------------------
// deletes the specified node pointed to by 'p' from the list
// ------------------------------------------------------------
void delete_ais_node(P_AIS_MESS p) {
	P_AIS_MESS temp;
	P_AIS_MESS prev;

	temp = p;
	prev = ais_head;

	if (temp == prev) {
		ais_head = ais_head->next;
		if (ais_end == temp)
			ais_end = ais_end->next;
	} else {
		while (prev->next != temp) {
			prev = prev->next;
		}
		prev->next = temp->next;
		if (ais_end == temp)
			ais_end = prev;
	}
	if (p->plmess != NULL)
		free(p->plmess);
	free(p);
}

// ------------------------------------------------------------
// initnode : Allocates a theads data structure
// ------------------------------------------------------------
P_TCP_SOCK init_node() {
	P_TCP_SOCK ptr;
	int pipestatus;

	ptr = (P_TCP_SOCK) malloc(sizeof(TCP_SOCK));

	memset(ptr, 0, sizeof(TCP_SOCK));

	if (ptr == NULL)
		return (P_TCP_SOCK) NULL;

	// Allocate pipe for publishing newly-arriving messages to
	// connected clients.
	pipestatus = pipe(ptr->msgpipe);
	if (pipestatus < 0) {
		perror("error allocating pipe");
		free(ptr);
		return (P_TCP_SOCK) NULL;
	}

	return ptr;
}

// ------------------------------------------------------------
// adding to end of list.
// ------------------------------------------------------------
void add_node(P_TCP_SOCK new_node) {
	pthread_mutex_lock(&lock);
	if (head == NULL) {
		head = new_node;
		end = new_node;
	}
	end->next = new_node;
	new_node->next = NULL;
	end = new_node;
	pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------
// deletes the specified node pointed to by 'p' from the list
// ------------------------------------------------------------
void delete_node(P_TCP_SOCK p) {
	P_TCP_SOCK temp;
	P_TCP_SOCK prev;

	pthread_mutex_lock(&lock);
	temp = p;
	prev = head;

	if (temp == prev) {
		head = head->next;
		if (end == temp)
			end = end->next;
	} else {
		while (prev->next != temp) {
			prev = prev->next;
		}
		prev->next = temp->next;
		if (end == temp)
			end = prev;
	}
	free(p);
	pthread_mutex_unlock(&lock);

}

// ------------------------------------------------------------------
// Return error category. Some errors we can live with, some we can't
// ------------------------------------------------------------------
int error_category(int rc) {
#if defined (__WIN32__)
	rc=rc; 	    // Not used, avoid compiling warnings
	return -1; // Just work as sis
#else
	switch (rc) {
	// Fatal errors
	case EINVAL:		 	// The listen function was not invoked prior to accept.
	case ENOTSOCK:	    	// The descriptor is not a socket.
	case EOPNOTSUPP:	    // The referenced socket is not a type that supports connection-oriented service.
	case EPROTONOSUPPORT: 	// The specified protocol is not supported.
	case EPROTOTYPE:
	case EFAULT: 			// The addrlen parameter is too small or addr is not a valid part of the user address space.
	case EADDRINUSE:	    // The specified address is already in use.
		if( _debug)
			fprintf( stderr, "Socket fatal error: %d\n", rc);
		return -1;

	// Retry errors
	case ENETDOWN:	    	// The network subsystem has failed.
	case EINTR:				// The (blocking) call was canceled through.
	case EINPROGRESS:		// A blocking call is in progress, or the service provider is still processing a callback function.
	case EMFILE:			// The queue is nonempty upon entry to accept and there are no descriptors available.
	case ENOBUFS:			// No buffer space is available.
	case EWOULDBLOCK:		// The socket is marked as nonblocking and no connections are present to be accepted.
	case EALREADY:			// A nonblocking connect call is in progress on the specified socket.
	case EADDRNOTAVAIL: 	// The specified address is not available from the local machine.
	case EAFNOSUPPORT: 		// Addresses in the specified family cannot be used with this socket.
	case ECONNREFUSED:    	// The attempt to connect was forcefully rejected.
	case EISCONN:    		// The socket is already connected (connection-oriented sockets only).
	case ENETUNREACH:    	// The network cannot be reached from this host at this time.
	case ETIMEDOUT:			// Attempt to connect timed out without establishing a connection.
	case EACCES:			// Attempt to connect datagram socket to broadcast address failed because setsockopt option SO_BROADCAST is not enabled.
	case ECONNRESET:
		if( _debug)
			fprintf( stderr, "Socket retry error: %d\n", rc);
		return -2;

	default:
		// Fatal error
		if( _debug)
			fprintf( stderr, "Socket unknown error: %d\n", rc);
		return -1;

	}
#endif 
}

