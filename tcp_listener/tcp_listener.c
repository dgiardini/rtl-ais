// ------------------------------------------------------------
// tcp_listener.c
// Written by Peter Schultz, hp@hpes.dk
// ------------------------------------------------------------
#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>

#if defined (__WIN32__)
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <netdb.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
#endif

typedef struct t_sockIo {
	int sock;
	pthread_t thread_t;
	int sesion_active;
	struct sockaddr_in cli_addr;
	char from_ip[20];
	struct t_sockIo *next;

} TCP_SOCK, *P_TCP_SOCK;

static int sockfd;
static int _debug_nmea = 0;
static int _debug = 0;
static int _tcp_keep_ais_time = 15;
static int portno;
pthread_mutex_t lock;

// Linked list vars.
P_TCP_SOCK head = (P_TCP_SOCK) NULL;
P_TCP_SOCK end = (P_TCP_SOCK) NULL;

pthread_t tcp_listener_thread;

typedef struct t_ais_mess {
	char message[100];  // max on nmea message is 83 char's
	int length;
	struct timeval timestamp;
	struct t_ais_mess *next;

} AIS_MESS, *P_AIS_MESS;

// Linked list ais messages.
P_AIS_MESS ais_head = (P_AIS_MESS) NULL;
P_AIS_MESS ais_end = (P_AIS_MESS) NULL;

pthread_mutex_t ais_lock;

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

int initTcpSocket(const char *portnumber, int debug_nmea, int tcp_keep_ais_time) {

	_debug_nmea = debug_nmea;
	_tcp_keep_ais_time = tcp_keep_ais_time;
	struct sockaddr_in serv_addr;

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "Failed to create socket! error %d\n", errno);
		return 0;
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	portno = atoi(portnumber);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);
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
	sleep(3000);
#else
	sleep(3);
#endif
	close(sockfd);

}

// ------------------------------------------------------------
// The main listener loop thread
// ------------------------------------------------------------
static void *tcp_listener_fn(void *arg) {
	int rc;
	P_TCP_SOCK t;

	fprintf(stderr, "Tcp listen port %d\nAis message timeout with %d\n", portno, _tcp_keep_ais_time);

	while (1) {

		t = init_node();

		rc = accept_c(t);

		if ( rc == -1)
			break;

		if (rc == -2) {
			close(t->sock);
			free(t);
			continue;
		}

		add_node(t);

		pthread_create(&t->thread_t, NULL, handle_remote_close, (void *) t);

	}
	shutdown( sockfd,2);
	close(sockfd);
}

// ------------------------------------------------------------
// thread func for hanling client close
// ------------------------------------------------------------
void *handle_remote_close(void *arg) {
	unsigned char buff[100];
	int rc;
	P_TCP_SOCK t = (P_TCP_SOCK) arg;
	P_AIS_MESS ais_temp;
	struct timeval timeout;
	timeout.tv_sec = 10;
	timeout.tv_usec = 0;
	setsockopt(t->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	// get rid of old messages before send
	remove_old_ais_messages();

	// send saved ais_messages to new socket
	ais_temp = ais_head;
	while (ais_temp != NULL) {
		rc = send(t->sock, ais_temp->message, ais_temp->length, 0);
		if( _debug)
			fprintf( stdout, "%d: Send to %s, <%.*s>, rc=%d\n",ais_temp->timestamp.tv_sec, t->from_ip, ais_temp->length, ais_temp->message, rc);
		ais_temp = ais_temp->next;
	}

	while(1){

		rc = recv(t->sock, buff, 99, 0);
		if( rc < 0) {
			if( _debug)
				fprintf( stdout, "Some socket error happend %d\n", errno);
			break;
		}
		else if( rc == 0) {
			if( _debug)
				fprintf( stdout, "client gracefully closed the socket\n");
	    	break;
		}
		else {
			if( _debug)
				fprintf( stdout, "Something receiced from client <%.*s>\n", rc, buff );
		}
	}
	close(t->sock);
	delete_node(t);
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
		if( errno	 == 22) return -1;
		return error_category(errno);
	}

	if (setsockopt(p_tcp_sock->sock, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen) < 0) {
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
	strncpy(new_node->message, mess, length);
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
	free(p);
}

// ------------------------------------------------------------
// initnode : Allocates a theads data structure
// ------------------------------------------------------------
P_TCP_SOCK init_node() {
	P_TCP_SOCK ptr;

	ptr = (P_TCP_SOCK) malloc(sizeof(TCP_SOCK));

	memset(ptr, 0, sizeof(TCP_SOCK));

	if (ptr == NULL)
		return (P_TCP_SOCK) NULL;
	else {
		return ptr;
	}
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
			fprintf( stderr, "Socket fatal error: %d\n", rc)
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
			fprintf( stderr, "Socket retry error: %d\n", rc)
		return -2;

	default:
		// Fatal error
		if( _debug)
			fprintf( stderr, "Socket unknown error: %d\n", rc)
		return -1;

	}
#endif 
}

