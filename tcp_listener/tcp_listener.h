// -------------------------------------------------------
// tcp_listener.h
// Written by Peter Schultz, hp@hpes.dk
// -------------------------------------------------------
#ifndef __TCP_LISTENER_H_
#define __TCP_LISTENER_H_

#define MAX_TCP_CONNECTIONS 100

// Prototypes
int initTcpSocket( const char *portnumber, int debug_nmea, int tcp_keep_ais_time, int tcp_stream_forever);
int add_nmea_ais_message(const char * mess, unsigned int length);
void closeTcpSocket();

#endif

