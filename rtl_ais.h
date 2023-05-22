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

#ifdef __cplusplus
extern "C" {
#endif

struct rtl_ais_config
{
    int gain, dev_index, dev_given, ppm_error, rtl_agc, custom_ppm;
    int left_freq, right_freq, sample_rate, output_rate, dongle_freq;
    int dongle_rate, delta, edge;

    int oversample, dc_filter, use_internal_aisdecoder;
    int seconds_for_decoder_stats;
    int use_tcp_listener, tcp_keep_ais_time, tcp_stream_forever;
    /* Aisdecoder */
    int	show_levels, debug_nmea;
    char *port, *host, *filename;

    int add_sample_num;
};

struct rtl_ais_context;

void rtl_ais_default_config(struct rtl_ais_config *config);
struct rtl_ais_context *rtl_ais_start(struct rtl_ais_config *config);
int rtl_ais_isactive(struct rtl_ais_context *ctx);
const char *rtl_ais_next_message(struct rtl_ais_context *ctx);
void rtl_ais_cleanup(struct rtl_ais_context *ctx);

#ifdef __cplusplus
}
#endif
