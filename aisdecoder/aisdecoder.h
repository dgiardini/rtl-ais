#ifndef __AIS_RL_AIS_INC_
#define  __AIS_RL_AIS_INC_
int init_ais_decoder(char * host, char * port,int show_levels,int _debug_nmea,int buf_len,int time_print_stats, int use_tcp_listener, int tcp_keep_ais_time, int tcp_stream_forever, int add_sample_num);
void run_rtlais_decoder(short * buff, int len);
const char *aisdecoder_next_message();
int free_ais_decoder(void);
#endif

