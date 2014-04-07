/*
 * Copyright (C) 2014 PHYTEC America, LLC
 * All rights reserved.
 * Author: Russell Robinson <rrobinson@phytec.com>
 *
 * Based on gps_qemu.c (AOSP) and gps_freerunner.c (Openmoko)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This code was originally ported from the Goldfish implementation for
 * the Android emulator
 *
 * this implements a GPS hardware library for the PHYTEC PCM049/KSP5012
 * the following code should be built as a shared library that will be
 * placed into /system/lib/hw/gps.pcm049.so
 *
 * it will be loaded by the code in hardware/libhardware/hardware.c
 * which is itself called from android_location_GpsLocationProvider.cpp
 */


#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <math.h>
#include <time.h>
#include <semaphore.h>
#include <signal.h>
#include <linux/socket.h>
#include <sys/socket.h>

#define  LOG_TAG  "gps_ksp5012"
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <hardware/gps.h>

/* the name of the ksp5012 uart/gps socket */
#define	KSP5012_CHANNEL_NAME  "/dev/ttyO0"
#define	JF2_PULSE_HIGH	1
#define	JF2_PULSE_LOW	0

#define	GPS_DEBUG  0

/* Since NMEA parser requires locks */
#define GPS_STATE_LOCK_FIX(_s)           \
	{                                        \
		int ret;                             \
		do {                                 \
			ret = sem_wait(&(_s)->fix_sem);  \
		} while (ret < 0 && errno == EINTR); \
	}

#define GPS_STATE_UNLOCK_FIX(_s)         \
	sem_post(&(_s)->fix_sem)

#define  DFR(...)   ALOGD(__VA_ARGS__)

#if GPS_DEBUG
#  define  D(...)   ALOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif

#define GPS_STATUS_CB(_cb, _s)    \
	if ((_cb).status_cb) {          \
		GpsStatus gps_status;         \
		gps_status.status = (_s);     \
		(_cb).status_cb(&gps_status); \
		DFR("gps status callback: 0x%x", _s); \
	}

#define GPS_DEV_SLOW_UPDATE_RATE (10)
#define GPS_DEV_HIGH_UPDATE_RATE (1)

#define GPS_DEV_LOW_BAUD  (B9600)
#define GPS_DEV_HIGH_BAUD (B38400)

#define  NMEA_MAX_SIZE  83

typedef void (*start_t)(void*);

enum {
	STATE_QUIT  = 0,
	STATE_INIT  = 1,
	STATE_START = 2
};

typedef struct {
	int     pos;
	int     overflow;
	int     utc_year;
	int     utc_mon;
	int     utc_day;
	int     utc_diff;
	int     sv_status_changed;
	GpsLocation  fix;
	GpsSvStatus  sv_status;
	gps_location_callback  callback;
	char    in[ NMEA_MAX_SIZE+1 ];
} NmeaReader;

/* this is the state of our connection */
typedef struct {
	int                     init;
	int                     fd;
	GpsCallbacks            callbacks;
	GpsStatus               gps_status;
	pthread_t               thread;
	pthread_t               tmr_thread;
	sem_t                   fix_sem;
	int                     control[2];
	char                    nmea_buf[512];
	int                     nmea_len;
	int                     min_interval;
	NmeaReader              reader;
} GpsState;

static void gps_dev_init(GpsState*  s);
static void gps_dev_deinit(int fd);
static void gps_dev_start(int fd);
static void gps_dev_stop(int fd);
static void gps_timer_thread( void*  arg );
static int  dev_tty_setup(GpsState*  s, int baud, int init);

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   T O K E N I Z E R                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

typedef struct {
	const char*  p;
	const char*  end;
} Token;

#define  MAX_NMEA_TOKENS  16

typedef struct {
	int     count;
	Token   tokens[ MAX_NMEA_TOKENS ];
} NmeaTokenizer;

static int
nmea_tokenizer_init( NmeaTokenizer*  t, const char*  p, const char*  end )
{
	int    count = 0;
	char*  q;

	// the initial '$' is optional
	if (p < end && p[0] == '$')
		p += 1;

	// remove trailing newline
	if (end > p && end[-1] == '\n') {
		end -= 1;
		if (end > p && end[-1] == '\r')
			end -= 1;
	}

	// get rid of checksum at the end of the sentecne
	if (end >= p+3 && end[-3] == '*') {
		end -= 3;
	}

	while (p < end) {
		const char*  q = p;

		q = memchr(p, ',', end-p);
		if (q == NULL)
			q = end;

		if (q > p) {
			if (count < MAX_NMEA_TOKENS) {
				t->tokens[count].p   = p;
				t->tokens[count].end = q;
				count += 1;
			}
		}
		if (q < end)
			q += 1;

		p = q;
	}

	t->count = count;

	return count;
}

static Token
nmea_tokenizer_get( NmeaTokenizer*  t, int  index )
{
	Token  tok;
	static const char*  dummy = "";

	if (index < 0 || index >= t->count) {
		tok.p = tok.end = dummy;
	} else
		tok = t->tokens[index];

	return tok;
}

static int
str2int( const char*  p, const char*  end )
{
	int   result = 0;
	int   len    = end - p;

	for ( ; len > 0; len--, p++ )
	{
		int  c;

		if (p >= end)
			goto Fail;

		c = *p - '0';
		if ((unsigned)c >= 10)
			goto Fail;

		result = result*10 + c;
	}

	return  result;

Fail:
	return -1;
}

static double
str2float( const char*  p, const char*  end )
{
	int   result = 0;
	int   len    = end - p;
	char  temp[16];

	if (len >= (int)sizeof(temp))
		return 0.;

	memcpy( temp, p, len );
	temp[len] = 0;

	return strtod( temp, NULL );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   P A R S E R                           *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

static void
nmea_reader_update_utc_diff( NmeaReader*  r )
{
	time_t         now = time(NULL);
	struct tm      tm_local;
	struct tm      tm_utc;
	long           time_local, time_utc;

	gmtime_r( &now, &tm_utc );
	localtime_r( &now, &tm_local );

	time_local = tm_local.tm_sec +
	60*(tm_local.tm_min +
	60*(tm_local.tm_hour +
	24*(tm_local.tm_yday +
	365*tm_local.tm_year)));

	time_utc = tm_utc.tm_sec +
	60*(tm_utc.tm_min +
	60*(tm_utc.tm_hour +
	24*(tm_utc.tm_yday +
	365*tm_utc.tm_year)));

	r->utc_diff = time_utc - time_local;
}

static void
nmea_reader_init( NmeaReader*  r )
{
	int i;
	memset( r, 0, sizeof(*r) );

	r->pos      = 0;
	r->overflow = 0;
	r->utc_year = -1;
	r->utc_mon  = -1;
	r->utc_day  = -1;
	r->callback = NULL;

	// Initialize the sizes of all the structs we use
	r->fix.size = sizeof(r->fix);
	r->sv_status.size = sizeof(r->sv_status);
	for (i = 0; i < GPS_MAX_SVS; i++) {
		r->sv_status.sv_list[i].size = sizeof(GpsSvInfo);
	}

	nmea_reader_update_utc_diff( r );
}

static void
nmea_reader_set_callback( NmeaReader*  r, gps_location_callback  cb )
{
	r->callback = cb;
	if (cb != NULL && r->fix.flags != 0) {
		D("%s: sending latest fix to new callback", __FUNCTION__);
		r->callback( &r->fix );
		r->fix.flags = 0;
	}
}

static int
nmea_reader_update_time( NmeaReader*  r, Token  tok )
{
	int        hour, minute, milliseconds;
	double     seconds;
	struct tm  tm;
	time_t     fix_time;

	if (tok.p + 6 > tok.end)
		return -1;

	if (r->utc_year < 0) {
		// no date yet, get current one
		time_t  now = time(NULL);
		gmtime_r( &now, &tm );
		r->utc_year = tm.tm_year + 1900;
		r->utc_mon  = tm.tm_mon + 1;
		r->utc_day  = tm.tm_mday;
	}

	hour    = str2int(tok.p,   tok.p+2);
	minute  = str2int(tok.p+2, tok.p+4);
	seconds = str2float(tok.p+4, tok.end);

	tm.tm_hour  = hour;
	tm.tm_min   = minute;
	tm.tm_sec   = (int) seconds;
	tm.tm_year  = r->utc_year - 1900;
	tm.tm_mon   = r->utc_mon - 1;
	tm.tm_mday  = r->utc_day;
	tm.tm_isdst = -1;

	fix_time = mktime( &tm ) + r->utc_diff;
	r->fix.timestamp = (long long)fix_time * 1000;

	return 0;
}

static
int nmea_reader_update_cdate( NmeaReader*  r, Token  tok_d, Token tok_m, Token tok_y )
{

	if ( (tok_d.p + 2 > tok_d.end) ||
			(tok_m.p + 2 > tok_m.end) ||
			(tok_y.p + 4 > tok_y.end) )
		return -1;

	r->utc_day = str2int(tok_d.p,   tok_d.p+2);
	r->utc_mon = str2int(tok_m.p, tok_m.p+2);
	r->utc_year = str2int(tok_y.p, tok_y.p+4);

	return 0;
}

static int
nmea_reader_update_date( NmeaReader*  r, Token  date, Token  time )
{
	Token  tok = date;
	int    day, mon, year;

	if (tok.p + 6 != tok.end) {
		D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	day  = str2int(tok.p, tok.p+2);
	mon  = str2int(tok.p+2, tok.p+4);
	year = str2int(tok.p+4, tok.p+6) + 2000;

	if ((day|mon|year) < 0) {
		D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}

	r->utc_year  = year;
	r->utc_mon   = mon;
	r->utc_day   = day;

	return nmea_reader_update_time( r, time );
}

static double
convert_from_hhmm( Token  tok )
{
	double  val     = str2float(tok.p, tok.end);
	int     degrees = (int)(floor(val) / 100);
	double  minutes = val - degrees*100.;
	double  dcoord  = degrees + minutes / 60.0;

	return dcoord;
}

static int
nmea_reader_update_latlong( NmeaReader*  r,
Token        latitude,
char         latitudeHemi,
Token        longitude,
char         longitudeHemi )
{
	double   lat, lon;
	Token    tok;

	tok = latitude;
	if (tok.p + 6 > tok.end) {
		D("latitude is too short: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	lat = convert_from_hhmm(tok);
	if (latitudeHemi == 'S')
		lat = -lat;

	tok = longitude;
	if (tok.p + 6 > tok.end) {
		D("longitude is too short: '%.*s'", tok.end-tok.p, tok.p);
		return -1;
	}
	lon = convert_from_hhmm(tok);
	if (longitudeHemi == 'W')
		lon = -lon;

	r->fix.flags    |= GPS_LOCATION_HAS_LAT_LONG;
	r->fix.latitude  = lat;
	r->fix.longitude = lon;

	return 0;
}

static int
nmea_reader_update_altitude( NmeaReader*  r,
Token        altitude,
Token        units )
{
	double  alt;
	Token   tok = altitude;

	if (tok.p >= tok.end)
		return -1;

	r->fix.flags   |= GPS_LOCATION_HAS_ALTITUDE;
	r->fix.altitude = str2float(tok.p, tok.end);

	return 0;
}

static int
nmea_reader_update_bearing( NmeaReader*  r,
Token        bearing )
{
	double  alt;
	Token   tok = bearing;

	if (tok.p >= tok.end)
		return -1;

	r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
	r->fix.bearing  = str2float(tok.p, tok.end);

	return 0;
}

static int
nmea_reader_update_speed( NmeaReader*  r,
Token        speed )
{
	double  alt;
	Token   tok = speed;

	if (tok.p >= tok.end)
		return -1;

	r->fix.flags   |= GPS_LOCATION_HAS_SPEED;
	r->fix.speed    = str2float(tok.p, tok.end);

	return 0;
}

static int
nmea_reader_update_accuracy( NmeaReader*  r,  Token accuracy)
{
	double acc;
	Token tok = accuracy;

	if (tok.p >= tok.end) {
		return -1;
	}

	r->fix.accuracy = str2float(tok.p, tok.end);

	if (r->fix.accuracy == 99.99) {
		return 0;
	}

	r->fix.flags   |= GPS_LOCATION_HAS_ACCURACY;

	return 0;
}

static void
nmea_reader_parse( NmeaReader*  r )
{
	/* we received a complete sentence, now parse it to generate
* a new GPS fix...
*/
	NmeaTokenizer  tzer[1];
	Token          tok;

	DFR("Received: '%.*s'", r->pos, r->in);
	if (r->pos < 9) {
		DFR("Too short. discarded.");
		return;
	}

	nmea_tokenizer_init(tzer, r->in, r->in + r->pos);
#if GPS_DEBUG
	{
		int  n;
		D("Found %d tokens", tzer->count);
		for (n = 0; n < tzer->count; n++) {
			Token  tok = nmea_tokenizer_get(tzer,n);
			D("%2d: '%.*s'", n, tok.end-tok.p, tok.p);
		}
	}
#endif

	tok = nmea_tokenizer_get(tzer, 0);
	if (tok.p + 5 > tok.end) {
		DFR("sentence id '%.*s' too short, ignored.", tok.end-tok.p, tok.p);
		return;
	}

	// ignore first two characters.
	tok.p += 2;
	if ( !memcmp(tok.p, "GGA", 3) ) {
		// GPS fix
		Token  tok_time          = nmea_tokenizer_get(tzer,1);
		Token  tok_latitude      = nmea_tokenizer_get(tzer,2);
		Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,3);
		Token  tok_longitude     = nmea_tokenizer_get(tzer,4);
		Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,5);
		Token  tok_altitude      = nmea_tokenizer_get(tzer,9);
		Token  tok_altitudeUnits = nmea_tokenizer_get(tzer,10);

		nmea_reader_update_time(r, tok_time);
		nmea_reader_update_latlong(r, tok_latitude,
		tok_latitudeHemi.p[0],
		tok_longitude,
		tok_longitudeHemi.p[0]);
		nmea_reader_update_altitude(r, tok_altitude, tok_altitudeUnits);
	} else if ( !memcmp(tok.p, "GLL", 3) ) {
		Token  tok_fixstaus      = nmea_tokenizer_get(tzer,6);

		if ((tok_fixstaus.p[0] == 'A') && (r->utc_year >= 0)) {
			// ignore this until we have a valid timestamp
			Token  tok_latitude      = nmea_tokenizer_get(tzer,1);
			Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,2);
			Token  tok_longitude     = nmea_tokenizer_get(tzer,3);
			Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,4);
			Token  tok_time          = nmea_tokenizer_get(tzer,5);

			nmea_reader_update_time(r, tok_time);
			nmea_reader_update_latlong(r, tok_latitude,
			tok_latitudeHemi.p[0],
			tok_longitude,
			tok_longitudeHemi.p[0]);
		}
	} else if ( !memcmp(tok.p, "GSA", 3) ) {

		Token  tok_fixStatus   = nmea_tokenizer_get(tzer, 2);
		int i;

		if (tok_fixStatus.p[0] != '\0' && tok_fixStatus.p[0] != '1') {

			Token  tok_accuracy      = nmea_tokenizer_get(tzer, 15);

			nmea_reader_update_accuracy(r, tok_accuracy);

			r->sv_status.used_in_fix_mask = 0ul;

			for (i = 3; i <= 14; ++i){

				Token  tok_prn  = nmea_tokenizer_get(tzer, i);
				int prn = str2int(tok_prn.p, tok_prn.end);

				if (prn > 0){
					r->sv_status.used_in_fix_mask |= (1ul << (32 - prn));
					r->sv_status_changed = 1;
					DFR("%s: fix mask is %d", __FUNCTION__, r->sv_status.used_in_fix_mask);
				}
			}
		}
	} else if ( !memcmp(tok.p, "GSV", 3) ) {
		Token  tok_noSatellites  = nmea_tokenizer_get(tzer, 3);
		int    noSatellites = str2int(tok_noSatellites.p, tok_noSatellites.end);

		if (noSatellites > 0) {
			Token  tok_noSentences   = nmea_tokenizer_get(tzer, 1);
			Token  tok_sentence      = nmea_tokenizer_get(tzer, 2);

			int sentence = str2int(tok_sentence.p, tok_sentence.end);
			int totalSentences = str2int(tok_noSentences.p, tok_noSentences.end);
			int curr;
			int i;

			if (sentence == 1) {
				r->sv_status_changed = 0;
				r->sv_status.num_svs = 0;
			}

			curr = r->sv_status.num_svs;

			i = 0;
			while (i < 4 && r->sv_status.num_svs < noSatellites){
				Token  tok_prn = nmea_tokenizer_get(tzer, i * 4 + 4);
				Token  tok_elevation = nmea_tokenizer_get(tzer, i * 4 + 5);
				Token  tok_azimuth = nmea_tokenizer_get(tzer, i * 4 + 6);
				Token  tok_snr = nmea_tokenizer_get(tzer, i * 4 + 7);

				r->sv_status.sv_list[curr].prn = str2int(tok_prn.p, tok_prn.end);
				r->sv_status.sv_list[curr].elevation = str2float(tok_elevation.p, tok_elevation.end);
				r->sv_status.sv_list[curr].azimuth = str2float(tok_azimuth.p, tok_azimuth.end);
				r->sv_status.sv_list[curr].snr = str2float(tok_snr.p, tok_snr.end);

				r->sv_status.num_svs += 1;

				curr += 1;
				i += 1;
			}

			if (sentence == totalSentences) {
				r->sv_status_changed = 1;
			}
			D("%s: GSV message with total satellites %d", __FUNCTION__, noSatellites);
		}
	} else if ( !memcmp(tok.p, "RMC", 3) ) {
		Token  tok_time          = nmea_tokenizer_get(tzer,1);
		Token  tok_fixStatus     = nmea_tokenizer_get(tzer,2);
		Token  tok_latitude      = nmea_tokenizer_get(tzer,3);
		Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,4);
		Token  tok_longitude     = nmea_tokenizer_get(tzer,5);
		Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,6);
		Token  tok_speed         = nmea_tokenizer_get(tzer,7);
		Token  tok_bearing       = nmea_tokenizer_get(tzer,8);
		Token  tok_date          = nmea_tokenizer_get(tzer,9);

		D("in RMC, fixStatus=%c", tok_fixStatus.p[0]);
		if (tok_fixStatus.p[0] == 'A')
		{
			nmea_reader_update_date( r, tok_date, tok_time );

			nmea_reader_update_latlong( r, tok_latitude,
			tok_latitudeHemi.p[0],
			tok_longitude,
			tok_longitudeHemi.p[0] );

			nmea_reader_update_bearing( r, tok_bearing );
			nmea_reader_update_speed  ( r, tok_speed );
		}

	} else if ( !memcmp(tok.p, "VTG", 3) ) {
		Token  tok_fixStatus     = nmea_tokenizer_get(tzer,9);

		if (tok_fixStatus.p[0] != '\0' && tok_fixStatus.p[0] != 'N')
		{
			Token  tok_bearing       = nmea_tokenizer_get(tzer,1);
			Token  tok_speed         = nmea_tokenizer_get(tzer,5);

			nmea_reader_update_bearing( r, tok_bearing );
			nmea_reader_update_speed  ( r, tok_speed );
		}
	} else if ( !memcmp(tok.p, "ZDA", 3) ) {
		Token  tok_time;
		Token  tok_year  = nmea_tokenizer_get(tzer,4);
		tok_time  = nmea_tokenizer_get(tzer,1);

		if ((tok_year.p[0] != '\0') && (tok_time.p[0] != '\0')) {
			// make sure to always set date and time together, lest bad things happen
			Token  tok_day   = nmea_tokenizer_get(tzer,2);
			Token  tok_mon   = nmea_tokenizer_get(tzer,3);

			nmea_reader_update_cdate( r, tok_day, tok_mon, tok_year );
			nmea_reader_update_time(r, tok_time);
		}
	} else {
		tok.p -= 2;
		D("unknown sentence '%.*s", tok.end-tok.p, tok.p);
	}

#if GPS_DEBUG
	if (r->fix.flags != 0) {
		char   temp[256];
		char*  p   = temp;
		char*  end = p + sizeof(temp);
		struct tm   utc;

		p += snprintf( p, end-p, "sending fix" );
		if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
			p += snprintf(p, end-p, " lat=%g lon=%g", r->fix.latitude, r->fix.longitude);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) {
			p += snprintf(p, end-p, " altitude=%g", r->fix.altitude);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_SPEED) {
			p += snprintf(p, end-p, " speed=%g", r->fix.speed);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_BEARING) {
			p += snprintf(p, end-p, " bearing=%g", r->fix.bearing);
		}
		if (r->fix.flags & GPS_LOCATION_HAS_ACCURACY) {
			p += snprintf(p,end-p, " accuracy=%g", r->fix.accuracy);
		}
		gmtime_r( (time_t*) &r->fix.timestamp, &utc );
		p += snprintf(p, end-p, " time=%s", asctime( &utc ) );
		D("temp: %s", temp);
	}
#endif
}

static void
nmea_reader_addc( NmeaReader*  r, int  c )
{
	if (r->overflow) {
		r->overflow = (c != '\n');
		return;
	}

	if (r->pos >= (int) sizeof(r->in)-1 ) {
		r->overflow = 1;
		r->pos      = 0;
		return;
	}

	r->in[r->pos] = (char)c;
	r->pos       += 1;

	if (c == '\n') {
		nmea_reader_parse( r );
		r->pos = 0;
	}
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       C O N N E C T I O N   S T A T E                 *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* commands sent to the gps thread */
enum {
	CMD_QUIT  = 0,
	CMD_START = 1,
	CMD_STOP  = 2
};

static GpsState  _gps_state[1];

static void
gps_state_done( GpsState*  s )
{
	// tell the thread to quit, and wait for it
	char   cmd = CMD_QUIT;
	void*  dummy;

	DFR("gps send quit command");

	write( s->control[0], &cmd, 1 );
	pthread_join(s->thread, &dummy);

	// close the control socket pair
	close( s->control[0] ); s->control[0] = -1;
	close( s->control[1] ); s->control[1] = -1;

	// close connection to the KSP5012 GPS daemon
	close( s->fd ); s->fd = -1;
	s->init = STATE_QUIT;
	s->min_interval = 1000;

	sem_destroy(&s->fix_sem);

	DFR("gps deinit complete");
}

static void
gps_state_start( GpsState*  s )
{
	char  cmd = CMD_START;
	int   ret;

	do {
		ret=write( s->control[0], &cmd, 1 );
	} while (ret < 0 && errno == EINTR);

	if (ret != 1)
		D("%s: could not send CMD_START command: ret=%d: %s",
			__FUNCTION__, ret, strerror(errno));
}

static void
gps_state_stop( GpsState*  s )
{
	char  cmd = CMD_STOP;
	int   ret;

	do {
		ret=write( s->control[0], &cmd, 1 );
	} while (ret < 0 && errno == EINTR);

	if (ret != 1)
		D("%s: could not send CMD_STOP command: ret=%d: %s",
			__FUNCTION__, ret, strerror(errno));
}

static int
epoll_register( int  epoll_fd, int  fd )
{
	struct epoll_event  ev;
	int                 ret, flags;

	/* important: make the fd non-blocking */
	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	ev.events  = EPOLLIN;
	ev.data.fd = fd;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fd, &ev );
	} while (ret < 0 && errno == EINTR);

	return ret;
}


static int
epoll_deregister( int  epoll_fd, int  fd )
{
	int  ret;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fd, NULL );
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static void gps_nmea_thread_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	state->callbacks.nmea_cb(state->reader.fix.timestamp,&state->nmea_buf[0],state->nmea_len);
	GPS_STATE_UNLOCK_FIX(state);
}

static void gps_nmea_cb( GpsState* state , const char* buf, int len)
{
	D("%s()", __FUNCTION__ );

	// Forward NMEA sentences ....
	if (state->callbacks.nmea_cb) {
		GPS_STATE_LOCK_FIX(state);
		memcpy(&state->nmea_buf[0],buf,len);
		state->nmea_buf[len] = 0;
		state->nmea_len = len;
		state->callbacks.create_thread_cb("nmea",(start_t)gps_nmea_thread_cb,(void*)state);
	}
}

static void gps_status_thread_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	state->callbacks.status_cb(&state->gps_status);
	GPS_STATE_UNLOCK_FIX(state);
}

static void gps_status_cb( GpsState* state , GpsStatusValue status)
{
	D("%s()", __FUNCTION__ );

	if (state->callbacks.status_cb) {
		GPS_STATE_LOCK_FIX(state);

		state->gps_status.size = sizeof(GpsStatus);
		state->gps_status.status = status;
		state->callbacks.create_thread_cb("status",(start_t)gps_status_thread_cb,(void*)state);

		D("gps status callback: 0x%x", status);
	}
}

static void gps_set_capabilities_cb( GpsState* state , uint32_t caps)
{
	D("%s()", __FUNCTION__ );

	if (state->callbacks.set_capabilities_cb) {
		state->callbacks.create_thread_cb("caps",(start_t)state->callbacks.set_capabilities_cb,(void*)caps);
	}
}

static void gps_location_thread_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	state->callbacks.location_cb( &state->reader.fix );
	state->reader.fix.flags = 0;
	GPS_STATE_UNLOCK_FIX(state);
}

static void gps_location_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	if (state->callbacks.location_cb) {
		GPS_STATE_LOCK_FIX(state);
		state->callbacks.create_thread_cb("fix",(start_t)gps_location_thread_cb,(void*)state);
	}
}

static void gps_sv_status_thread_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	state->callbacks.sv_status_cb( &state->reader.sv_status );
	state->reader.sv_status_changed = 0;
	GPS_STATE_UNLOCK_FIX(state);
}

static void gps_sv_status_cb( GpsState* state )
{
	D("%s()", __FUNCTION__ );

	if (state->callbacks.sv_status_cb) {
		GPS_STATE_LOCK_FIX(state);
		state->callbacks.create_thread_cb("sv-status",(start_t)gps_sv_status_thread_cb,(void*)state);
	}
}

/* this is the main thread, it waits for commands from gps_state_start/stop and,
* when started, messages from the KSP5012 GPS daemon. these are simple NMEA sentences
* that must be parsed to be converted into GPS fixes sent to the framework
*/
static void
gps_state_thread( void*  arg )
{
	GpsState*   state = (GpsState*) arg;
	NmeaReader  *reader;
	int         epoll_fd   = epoll_create(2);
	int         started    = 0;
	int         gps_fd     = state->fd;
	int         control_fd = state->control[1];

	reader = &state->reader;
	nmea_reader_init( reader );

	// register control file descriptors for polling
	epoll_register( epoll_fd, control_fd );
	epoll_register( epoll_fd, gps_fd );

	D("gps thread running");

	gps_dev_init(state);

	gps_set_capabilities_cb( state , GPS_CAPABILITY_MSA | GPS_CAPABILITY_MSB );

	D("after set capabilities");

	GPS_STATUS_CB(state->callbacks, GPS_STATUS_ENGINE_ON);

	// now loop
	for (;;) {
		struct epoll_event   events[2];
		int                  ne, nevents;

		nevents = epoll_wait( epoll_fd, events, 2, -1 );
		if (nevents < 0) {
			if (errno != EINTR)
			ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
			continue;
		}
		D("gps thread received %d events", nevents);
		for (ne = 0; ne < nevents; ne++) {
			if ((events[ne].events & (EPOLLERR|EPOLLHUP)) != 0) {
				ALOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
				goto deinit;
			}
			if ((events[ne].events & EPOLLIN) != 0) {
				int  fd = events[ne].data.fd;

				if (fd == control_fd)
				{
					char  cmd = 255;
					int   ret;
					D("gps control fd event");
					do {
						ret = read( fd, &cmd, 1 );
					} while (ret < 0 && errno == EINTR);

					if (cmd == CMD_QUIT) {
						D("gps thread quitting on demand");
						goto deinit;
					}
					else if (cmd == CMD_START) {
						if (!started) {
							D("gps thread starting  location_cb=%p", state->callbacks.location_cb);
							started = 1;

							gps_dev_start(gps_fd);
							GPS_STATUS_CB(state->callbacks, GPS_STATUS_SESSION_BEGIN);
							state->init = STATE_START;

							state->tmr_thread = state->callbacks.create_thread_cb(
							"gps_timer_thread", gps_timer_thread, state );

							if ( !state->tmr_thread ) {
								ALOGE("could not create gps timer thread: %s", strerror(errno));

								started = 0;
								state->init = STATE_INIT;
								goto deinit;
							}

						}
					}
					else if (cmd == CMD_STOP) {
						if (started) {
							void*  dummy;

							D("gps thread stopping");
							started = 0;

							gps_dev_stop(gps_fd);
							state->init = STATE_INIT;
							pthread_join(state->tmr_thread, &dummy);
							GPS_STATUS_CB(state->callbacks, GPS_STATUS_SESSION_END);

						}
					}
				}
				else if (fd == gps_fd)
				{
					char  buff[32];
					D("gps fd event");
					for (;;) {
						int  nn, ret;

						ret = read( fd, buff, sizeof(buff) );
						if (ret < 0) {
							if (errno == EINTR)
							continue;
							if (errno != EWOULDBLOCK)
							ALOGE("error while reading from gps daemon socket: %s:", strerror(errno));
							break;
						}
						D("received %d bytes: %.*s", ret, ret, buff);

						gps_nmea_cb ( state, &buff[0], ret);

						GPS_STATE_LOCK_FIX(state);
						for (nn = 0; nn < ret; nn++)
							nmea_reader_addc( reader, buff[nn] );
						GPS_STATE_UNLOCK_FIX(state);
					}
					D("gps fd event end");
				}
				else
				{
					ALOGE("epoll_wait() returned unkown fd %d ?", fd);
				}
			}
		}
	}
deinit:
	GPS_STATUS_CB(state->callbacks, GPS_STATUS_ENGINE_OFF);
	gps_dev_deinit(gps_fd);

	return;
}

static void gps_timer_thread( void*  arg )
{

	GpsState *state = (GpsState *)arg;

	D("gps entered timer thread");

	do {
		D ("gps timer exp");

		if (state->reader.fix.flags != 0) {
			D("gps fix cb: 0x%x", state->reader.fix.flags);
			gps_location_cb( state );
		}

		if (state->reader.sv_status_changed != 0) {
			D("gps sv status callback");
			gps_sv_status_cb( state );
		}

		if (state->min_interval == 0) {
			state->min_interval = 1000;
		}
		usleep(state->min_interval*1000);

	} while(state->init == STATE_START);

	D("gps timer thread destroyed");

	return;
}

static void
gps_state_init( GpsState*  state, GpsCallbacks* callbacks )
{
	state->init       = STATE_INIT;
	state->control[0] = -1;
	state->control[1] = -1;
	state->fd         = -1;
	state->min_interval = 1000;

	state->fd = open(KSP5012_CHANNEL_NAME, O_RDWR);

	if (state->fd < 0) {
		DFR("%s: Could not open %s", __FUNCTION__, KSP5012_CHANNEL_NAME);
		return;
	}

	if (dev_tty_setup(state, 0, 1) < 0) {
		ALOGE("could not perform initial tty setup");
		goto Fail;
	}

	if (sem_init(&state->fix_sem, 0, 1) != 0) {
		DFR("gps semaphore initialization failed! errno = %d", errno);
		return;
	}

	if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, state->control ) < 0 ) {
		ALOGE("could not create thread control socket pair: %s", strerror(errno));
		goto Fail;
	}

	state->thread = callbacks->create_thread_cb( "gps_state_thread", gps_state_thread, state );


	if ( !state->thread ) {
		ALOGE("could not create gps thread: %s", strerror(errno));
		goto Fail;
	}

	state->callbacks = *callbacks;

	DFR("gps state initialized");

	return;

Fail:
	gps_state_done( state );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       I N T E R F A C E                               *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

char const *const GPS_ONOFF_FILE = "/sys/class/gpio/gpio172/value";

static int
write_int(char const *path, int value)
{
	int fd;
	static int already_warned;

	fd = open(path, O_RDWR);
	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%d\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);

		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			ALOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}

		return -errno;
	}
}

static int
ksp5012_gps_init(GpsCallbacks* callbacks)
{
	GpsState*  s = _gps_state;

	if (!s->init) {
		gps_state_init(s, callbacks);
	}

	if (s->fd < 0)
		return -1;

	return 0;
}

static void
ksp5012_gps_cleanup(void)
{
	GpsState*  s = _gps_state;

	if (s->init) {
		gps_state_done(s);
	}
}

static int
ksp5012_gps_start()
{
	GpsState*  s = _gps_state;

	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	D("%s: called", __FUNCTION__);

	gps_state_start(s);

	return 0;
}

static int
ksp5012_gps_stop()
{
	GpsState*  s = _gps_state;

	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	D("%s: called", __FUNCTION__);

	gps_state_stop(s);

	return 0;
}

static int
ksp5012_gps_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty)
{
	return 0;
}

static int
ksp5012_gps_inject_location(double latitude, double longitude, float accuracy)
{
	return 0;
}

static void
ksp5012_gps_delete_aiding_data(GpsAidingData flags)
{
	return;
}

static int
ksp5012_gps_set_position_mode(GpsPositionMode mode, GpsPositionRecurrence recurrence,
uint32_t min_interval, uint32_t preferred_accuracy, uint32_t preferred_time)
{
	return 0;
}

static const void*
ksp5012_gps_get_extension(const char* name)
{
	// no extensions supported
	return NULL;
}

static const GpsInterface ksp5012GpsInterface = {
	sizeof(GpsInterface),
	ksp5012_gps_init,
	ksp5012_gps_start,
	ksp5012_gps_stop,
	ksp5012_gps_cleanup,
	ksp5012_gps_inject_time,
	ksp5012_gps_inject_location,
	ksp5012_gps_delete_aiding_data,
	ksp5012_gps_set_position_mode,
	ksp5012_gps_get_extension,
};

const GpsInterface* gps__get_gps_interface(struct gps_device_t* dev)
{
	return &ksp5012GpsInterface;
}

static int open_gps(const struct hw_module_t* module, char const* name,
struct hw_device_t** device)
{
	struct gps_device_t *dev = malloc(sizeof(struct gps_device_t));
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t*)module;
	dev->get_gps_interface = gps__get_gps_interface;

	*device = (struct hw_device_t*)dev;

	return 0;
}

static struct hw_module_methods_t gps_module_methods = {
	.open = open_gps
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 1,
	.version_minor = 0,
	.id = GPS_HARDWARE_MODULE_ID,
	.name = "KSP5012 JF2 GPS Module",
	.author = "The Android Open Source Project",
	.methods = &gps_module_methods,
};

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       D E V I C E                                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

static void gps_dev_power(int state)
{
	/* Toggle GPS Module */
	write_int(GPS_ONOFF_FILE, JF2_PULSE_HIGH);
	usleep(110);
	write_int(GPS_ONOFF_FILE, JF2_PULSE_LOW);

	usleep(1000*1000);

	DFR("gps power state = %d", state);

	return;
}

static void gps_dev_send(int fd, char *msg)
{
	int i, n, ret;

	i = strlen(msg);
	n = 0;

	do {
		ret = write(fd, msg + n, i - n);
		if (ret < 0 && errno == EINTR) {
			continue;
		}

		n += ret;
	} while (n < i);

	return;
}

static unsigned char gps_dev_calc_nmea_csum(char *msg)
{
	unsigned char csum = 0;
	int i;

	for (i = 1; msg[i] != '*'; ++i) {
		csum ^= msg[i];
	}

	return csum;
}

static void gps_dev_set_nmea_message_rate(int fd, int msg, int rate)
{
	char buff[50];
	int i;

	sprintf(buff, "$PSRF103,%.2d,00,%.2d,01*", msg, rate);
	i = strlen(buff);
	sprintf((buff + i), "%02x\r\n", gps_dev_calc_nmea_csum(buff));

	gps_dev_send(fd, buff);

	D("%s: msg rate sent to device: %s", __FUNCTION__, buff);

	return;
}

static void gps_dev_set_baud_rate(int fd, int baud)
{
	char buff[50];
	int i;

	sprintf(buff, "$PSRF100,1,%d,8,1,0*", baud);
	i = strlen(buff);
	sprintf((buff + i), "%02x\r\n", gps_dev_calc_nmea_csum(buff));

	gps_dev_send(fd, buff);

	D("%s: baud rate sent to device: %s", __FUNCTION__, buff);

	return;
}

static void gps_dev_set_message_rate(int fd, int rate)
{
	unsigned int i;
	enum msg_types {
		GGA = 0,
		GLL = 1,
		GSA = 2,
		GSV = 3,
		RMC = 4,
		VTG = 5,
		ZDA = 8,
	};
	int msg[] = { GGA, GLL, GSA, GSV, RMC, VTG, ZDA };

	for (i = 0; i < sizeof(msg)/sizeof(msg[0]); ++i) {
		gps_dev_set_nmea_message_rate(fd, msg[i], rate);
	}

	return;
}

static void gps_dev_init(GpsState*  s)
{
	gps_dev_power(1);

	return;
}

static void gps_dev_deinit(int fd)
{
	gps_dev_power(0);

	return;
}

static void gps_dev_start(int fd)
{
	D("gps dev start initiated");

	return;
}

static void gps_dev_stop(int fd)
{
	D("gps dev stop initiated");

	return;
}

static int dev_tty_setup(GpsState* s, int baud, int init)
{
	struct termios options;
	int ret = 0;

	if ((ret = tcgetattr(s->fd, &options)) < 0) {
		DFR("%s: failed to get tty attr", __FUNCTION__);
		goto exit;
	}

	if (init) {
		options.c_oflag &= ~ONLCR;

		options.c_iflag &= ~(ICRNL | INLCR | IXON);
		options.c_iflag |= (IGNCR | IGNBRK | IGNPAR);

		options.c_cflag &= ~(CRTSCTS | PARENB | CSIZE);
		options.c_cflag |= (CLOCAL | CREAD | B9600 | CS8
		| CSTOPB);

		if ((ret = tcsetattr(s->fd, TCSANOW, &options)) < 0) {
			DFR("%s: failed to set tty attr", __FUNCTION__);
			goto exit;
		}
	}

exit:
	return ret;
}
