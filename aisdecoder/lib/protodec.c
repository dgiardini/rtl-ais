
/*
 *	protodec.c
 *
 *	(c) Ruben Undheim 2008
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <time.h>
#include <string.h>		/* String function definitions */
#include "callbacks.h"
//#include "config.h"

#include "protodec.h"
#include "hmalloc.h"

decoder_on_nmea_sentence_received on_nmea_sentence_received=NULL;

#ifdef DMALLOC
#include <dmalloc.h>
#endif

void protodec_initialize(struct demod_state_t *d, struct serial_state_t *serial, char chanid, int add_sample_num)
{
	memset(d, 0, sizeof(struct demod_state_t));

	d->chanid = chanid;
	d->serial = serial;
	
	d->receivedframes = 0;
	d->lostframes = 0;
	d->lostframes2 = 0;
	
	protodec_reset(d);
	
	d->seqnr = 0;
    d->add_sample_num = add_sample_num;
	
    d->buffer = hmalloc(DEMOD_BUFFER_LEN);
    d->rbuffer = hmalloc(DEMOD_BUFFER_LEN);
    d->nmea = hmalloc(NMEABUFFER_LEN);
}

void protodec_deinit(struct demod_state_t *d)
{
	hfree(d->buffer);
	hfree(d->rbuffer);
    hfree(d->nmea);
}

void protodec_reset(struct demod_state_t *d)
{
	d->state = ST_SKURR;
	d->nskurr = 0;
	d->ndata = 0;
	d->npreamble = 0;
	d->nstartsign = 0;
	d->nstopsign = 0;
	d->antallpreamble = 0;
	d->antallenner = 0;
	d->last = 0;
	d->bitstuff = 0;
	d->bufferpos = 0;
}

/*
 * Calculates CRC-checksum
 */
 
unsigned short protodec_sdlc_crc(const unsigned char *data, unsigned len)
{
	unsigned short c, crc = 0xffff;

	while (len--)
		for (c = 0x100 + *data++; c > 1; c >>= 1)
			if ((crc ^ c) & 1)
				crc = (crc >> 1) ^ 0x8408;
			else
				crc >>= 1;
	return ~crc;

}

int protodec_calculate_crc(int length_bits, struct demod_state_t *d)
{
	int length_bytes;
	unsigned char *buf;
	int buflen;
	int i, j, x;
	unsigned char tmp;
	
	if (length_bits <= 0) {
		return 0;
	}
	
	length_bytes = length_bits / 8;
	buflen = length_bytes + 2;
	
	/* what is this? */
	buf = (unsigned char *) hmalloc(sizeof(*buf) * buflen);
	for (j = 0; j < buflen; j++) {
		tmp = 0;
		for (i = 0; i < 8; i++)
			tmp |= (((d->buffer[i + 8 * j]) << (i)));
		buf[j] = tmp;
	}
	
	/* ok, here's the actual CRC calculation */
	unsigned short crc = protodec_sdlc_crc(buf, buflen);
	//DBG(printf("CRC: %04x\n",crc));
	
	/* what is this? */
	memset(d->rbuffer, 0, DEMOD_BUFFER_LEN);
	for (j = 0; j < length_bytes; j++) {
		for (i = 0; i < 8; i++) {
			x = j * 8 + i;
			if (x >= DEMOD_BUFFER_LEN) {
				hfree(buf);
				return 0;
			} else {
				d->rbuffer[x] = (buf[j] >> (7 - i)) & 1;
			}
		}
	}
	
	hfree(buf);
	
	return (crc == 0x0f47);
}

unsigned long protodec_henten(int from, int size, unsigned char *frame)
{
	int i = 0;
	unsigned long tmp = 0;
	
	for (i = 0; i < size; i++)
		tmp |= (frame[from + i]) << (size - 1 - i);
	
	return tmp;
}


void protodec_generate_nmea(struct demod_state_t *d, int bufferlen, int fillbits, time_t received_t)
{
    int senlen;
    int pos;
    int k, offset;
    int m;
    int inc;

    unsigned char sentences, sentencenum, nmeachk, letter;
    (void)(received_t); // not used here, avoid compiling warnings
	//6bits to nmea-ascii. One sentence len max 82char
	//inc. head + tail.This makes inside datamax 62char multipart, 62 single
    senlen = 56;		//this is normally not needed.For testing only. May be fixed number
    if (bufferlen <= (senlen * 6)) {
		sentences = 1;
	} else {
		sentences = bufferlen / (senlen * 6);
		//sentences , if overflow put one more
		if (bufferlen % (senlen * 6) != 0)
			sentences++;
	};

    sentencenum = 0;
	pos = 0;
    offset = (sentences>1) ? 15 : 14;
	do {
        k = offset;		//leave room for nmea header
        while (k < senlen + offset && bufferlen > pos) {
            letter = (unsigned char)protodec_henten(pos, 6, d->rbuffer);
			// 6bit-to-ascii conversion by IEC
            letter += (letter < 40) ? 48 : 56;
			d->nmea[k] = letter;
			pos += 6;
			k++;
		}
		sentencenum++;
		
        memcpy(&d->nmea[0], "!AIVDM,0,0,", 11);
        d->nmea[7] += sentences;
        d->nmea[9] += sentencenum;

        memcpy(&d->nmea[k], ",0*00\0", 6);
		
		if (sentences > 1) {
            d->nmea[11] = '0' + d->seqnr;
            d->nmea[12] = ',';
            d->nmea[13] = d->chanid;
            d->nmea[14] = ',';
            if (sentencenum == sentences) d->nmea[k + 1] = '0' + fillbits;
        } else {
            d->nmea[11] = ',';
            d->nmea[12] = d->chanid;
            d->nmea[13] = ',';
		}

        m = 1;
        nmeachk = d->nmea[m++];
        while (d->nmea[m] != '*') nmeachk ^= d->nmea[m++];

        if (d->add_sample_num){
            inc = sprintf(&d->nmea[k + 3], "%02X,%lu\r\n", nmeachk, d->startsample);
        }else{
            inc = sprintf(&d->nmea[k + 3], "%02X\r\n", nmeachk);
        }
        if (on_nmea_sentence_received != NULL)
            on_nmea_sentence_received(d->nmea, k+3+inc, sentences, sentencenum);
    } while (sentencenum < sentences);
}

void protodec_getdata(int bufferlen, struct demod_state_t *d)
{
	unsigned char type = protodec_henten(0, 6, d->rbuffer);
	if (type < 1 || type > MAX_AIS_PACKET_TYPE /* 9 */)
		return;
//	unsigned long mmsi = protodec_henten(8, 30, d->rbuffer);
	int fillbits = 0;
	int k;
	time_t received_t;
	time(&received_t);
	
	if (bufferlen % 6 > 0) {
		fillbits = 6 - (bufferlen % 6);
		for (k = bufferlen; k < bufferlen + fillbits; k++)
			d->rbuffer[k] = 0;
		
		bufferlen = bufferlen + fillbits;
	}

	protodec_generate_nmea(d, bufferlen, fillbits, received_t);
	
	d->seqnr++;
	if (d->seqnr > 9)
		d->seqnr = 0;
	
	if (type < 1 || type > MAX_AIS_PACKET_TYPE)
		return; // unsupported packet type
}

void protodec_decode(char *in, int count, struct demod_state_t *d, unsigned long samplenum)
{
	int i = 0;
	int bufferlength, correct;
	
	while (i < count) {
		switch (d->state) {
		case ST_DATA:
			if (d->bitstuff) {
				if (in[i] == 1) {
					d->state = ST_STOPSIGN;
					d->ndata = 0;
					d->bitstuff = 0;
				} else {
					d->ndata++;
					d->last = in[i];
					d->bitstuff = 0;
				}
			} else {
				if (in[i] == d->last && in[i] == 1) {
					d->antallenner++;
					if (d->antallenner == 4) {
						d->bitstuff = 1;
						d->antallenner = 0;
					}

				} else
					d->antallenner = 0;

				d->buffer[d->bufferpos] = in[i];
				d->bufferpos++;
				d->ndata++;
				
				if (d->bufferpos >= 449) {
					protodec_reset(d);
				}
			}
			break;

        case ST_SKURR:
			if (in[i] != d->last)
				d->antallpreamble++;
			else
				d->antallpreamble = 0;
			d->last = in[i];
			if (d->antallpreamble > 14 && in[i] == 0) {
				d->state = ST_PREAMBLE;
				d->nskurr = 0;
				d->antallpreamble = 0;
			}
			d->nskurr++;
			break;

        case ST_PREAMBLE:
			if (in[i] != d->last && d->nstartsign == 0) {
				d->antallpreamble++;
			} else {
                if (in[i] == 1)	{
                    if (d->nstartsign == 0) {
						d->nstartsign = 3;
						d->last = in[i];
                    } else if (d->nstartsign == 5) {
						d->nstartsign++;
						d->npreamble = 0;
						d->antallpreamble = 0;
						d->state = ST_STARTSIGN;
					} else {
						d->nstartsign++;
					}

                } else {
					if (d->nstartsign == 0) {
						d->nstartsign = 1;
					} else {
						protodec_reset(d);
					}
				}
			}
			d->npreamble++;
			break;

		case ST_STARTSIGN:
			if (d->nstartsign >= 7) {
				if (in[i] == 0) {
					d->state = ST_DATA;
                    d->startsample = samplenum;
					d->nstartsign = 0;
					d->antallenner = 0;
					memset(d->buffer, 0, DEMOD_BUFFER_LEN);
					d->bufferpos = 0;
				} else {
					protodec_reset(d);
				}

			} else if (in[i] == 0) {
				protodec_reset(d);
			}
			d->nstartsign++;
			break;

		case ST_STOPSIGN:
			bufferlength = d->bufferpos - 6 - 16;
			if (in[i] == 0 && bufferlength > 0) {
				correct = protodec_calculate_crc(bufferlength, d);
				if (correct) {
					d->receivedframes++;
					protodec_getdata(bufferlength, d);
				} else {
					d->lostframes++;
				}
			} else {
				d->lostframes2++;
			}
			protodec_reset(d);
			break;


		}
		d->last = in[i];
		i++;
	}
}

