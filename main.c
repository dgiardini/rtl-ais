/*
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

typedef void* rtlsdr_dev_t;
#include "convenience.h"
#include "rtl_ais.h"

void usage(void)
{
	fprintf(stderr,
		"rtl_ais, a simple AIS tuner\n"
		"\t and generic dual-frequency FM demodulator\n\n"
		"(probably not a good idea to use with e4000 tuners)\n"
		"Use: rtl_ais [options] [outputfile]\n"
		"\t[-l left_frequency (default: 161.975M)]\n"
		"\t[-r right_frequency (default: 162.025M)]\n"
		"\t    left freq < right freq\n"
		"\t    frequencies must be within 1.2MHz\n"
		"\t[-s sample_rate (default: 24k)]\n"
		"\t    maximum value, might be down to 12k\n"
		"\t[-o output_rate (default: 48k)]\n"
		"\t    must be equal or greater than twice -s value\n"
		"\t[-E toggle edge tuning (default: off)]\n"
		"\t[-D toggle DC filter (default: on)]\n"
		//"\t[-O toggle oversampling (default: off)\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-g tuner_gain (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-R enable RTL chip AGC (default: off)]\n"
		"\t[-A turn off built-in AIS decoder (default: on)]\n"
		"\t    use this option to output samples to file or stdout.\n"
		"\tBuilt-in AIS decoder options:\n"
		"\t[-h host (default: 127.0.0.1)]\n"
		"\t[-P port (default: 10110)]\n"
		"\t[-T use TCP communication, rtl-ais is tcp server ( -h is ignored)\n"
		"\t[-t time to keep ais messages in sec, using tcp listener (default: 15)\n"
		"\t[-n log NMEA sentences to console (stderr) (default off)]\n"
		"\t[-I add sample index to NMEA messages (default off)]\n"
		"\t[-L log sound levels to console (stderr) (default off)]\n\n"
		"\t[-S seconds_for_decoder_stats (default 0=off)]\n\n"
		"\tWhen the built-in AIS decoder is disabled the samples are sent to\n"
		"\tto [outputfile] (a '-' dumps samples to stdout)\n"
		"\t    omitting the filename also uses stdout\n\n"
		"\tOutput is stereo 2x16 bit signed ints\n\n"
		"\tExamples:\n"
		"\tReceive AIS traffic,sent UDP NMEA sentences to 127.0.0.1 port 10110\n"
		"\t     and log the senteces to console:\n\n"
		"\trtl_ais -n\n\n"
		"\tTune two fm stations and play one on each channel:\n\n"
		"\trtl_ais -l233.15M  -r233.20M -A  | play -r48k -traw -es -b16 -c2 -V1 - "
		"\n");
	exit(1);
}

static volatile int do_exit = 0;
static void sighandler(int signum)
{
        (void)(signum); // unused argument
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
}

int main(int argc, char **argv)
{
#ifndef WIN32
	struct sigaction sigact;

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#endif
        int opt;
	
        struct rtl_ais_config config;
        rtl_ais_default_config(&config);

        config.host = strdup("127.0.0.1");
        config.port = strdup("10110");
        
	while ((opt = getopt(argc, argv, "l:r:s:o:EODd:g:p:RATIt:P:h:nLS:?")) != -1)
	{
		switch (opt) {
		case 'l':
                        config.left_freq = (int)atofs(optarg);
			break;
		case 'r':
			config.right_freq = (int)atofs(optarg);
			break;
		case 's':
			config.sample_rate = (int)atofs(optarg);
                        break;
		case 'o':
			config.output_rate = (int)atofs(optarg);
			break;
		case 'E':
			config.edge = !config.edge;
			break;
		case 'D':
			config.dc_filter = !config.dc_filter;
			break;
		case 'O':
			config.oversample = !config.oversample;
			break;
		case 'd':
			config.dev_index = verbose_device_search(optarg);
			config.dev_given = 1;
			break;
		case 'g':
			config.gain = (int)(atof(optarg) * 10);
			break;
		case 'p':
			config.ppm_error = atoi(optarg);
			config.custom_ppm = 1;
			break;
		case 'R':
			config.rtl_agc=1;
			break;
		case 'A':
			config.use_internal_aisdecoder=0;
			break;
        case 'I':
            config.add_sample_num = 1;
            break;
		case 'P':
			config.port=strdup(optarg);
			break;
                case 'T':
                        config.use_tcp_listener=1;
                        break;
                case 't':
                        config.tcp_keep_ais_time = atoi(optarg);
                        break;
		case 'h':
			config.host=strdup(optarg);
			break;
		case 'L':
			config.show_levels=1;
			break;
		case 'S':
			config.seconds_for_decoder_stats=atoi(optarg);
			break;
		case 'n':
			config.debug_nmea = 1;
			break;
		case '?':
		default:
			usage();
			return 2;
		}
	}

	if (argc <= optind) {
		config.filename = "-";
	} else {
		config.filename = argv[optind];
	}

	if (config.edge) {
		fprintf(stderr, "Edge tuning enabled.\n");
	} else {
		fprintf(stderr, "Edge tuning disabled.\n");
	}
	if (config.dc_filter) {
		fprintf(stderr, "DC filter enabled.\n");
	} else {
		fprintf(stderr, "DC filter disabled.\n");
	}
	if (config.rtl_agc) {
		fprintf(stderr, "RTL AGC enabled.\n");
	} else {
		fprintf(stderr, "RTL AGC disabled.\n");
	}
	if (config.use_internal_aisdecoder) {
		fprintf(stderr, "Internal AIS decoder enabled.\n");
	} else {
		fprintf(stderr, "Internal AIS decoder disabled.\n");
	}

        struct rtl_ais_context *ctx = rtl_ais_start(&config);
        if(!ctx) {
                fprintf(stderr, "\nrtl_ais_start failed, exiting...\n");
                exit(1);
        }
        	/* 
	  aidecoder.c appends the messages to a queue that can be used for a 
	  routine if rtl_ais is compiled as lib. Here we only loop and dequeue
	  the messages, and the puts() sentence that print the message is  
	  commented out. If the -n parameter is used the messages are printed from 
	  nmea_sentence_received() in aidecoder.c 
	  */
	while(!do_exit && rtl_ais_isactive(ctx)) {
	#if _POSIX_C_SOURCE >= 199309L // nanosleep available()
		struct timespec five = { 0, 50 * 1000 * 1000};
	#endif
		const char *str;
		if(config.use_internal_aisdecoder)
		{
			// dequeue
			while((str = rtl_ais_next_message(ctx)))
			{
				//puts(str); or code something that fits your needs
			}
		}
	#if _POSIX_C_SOURCE >= 199309L // nanosleep available()
		nanosleep(&five, NULL);
	#else
		usleep(50000);
	#endif
        }
        rtl_ais_cleanup(ctx);
        return 0;
}
