/*
 * slicctl - bring-up tool for the Le9641 SLIC on the ZLT S12 Pro.
 *
 *   slicctl id              raw MPI read of RCN/PCN (expects PCN 0x49)
 *   slicctl clk             set PCLK select, report the CFAIL (clock fault) bit
 *   slicctl ring [sec]      init + calibrate, ring for sec seconds
 *   slicctl tone [sec]      init, line ACTIVE, internal tone generator on
 *   slicctl monitor [sec]   print hook events; tone while off-hook
 *   slicctl talk [sec]      init, line TALK, 16-bit linear PCM on timeslot 0
 *   slicctl cidgen FMT NUM [NAME]  write a caller-ID signal (s16 8 kHz) to stdout;
 *                           FMT = bell | v23 | dtmf (no hardware access)
 *   slicctl dtmfdet         read s16 8 kHz audio on stdin, print detected
 *                           keypad digits (no hardware access)
 *   slicctl daemon          run the phone line (see the daemon section below):
 *                           dial tone and digit collection while off-hook,
 *                           calls bridged to Asterisk over AudioSocket,
 *                           commands on the FIFO /var/run/slic.ctl:
 *                             ring [sec [number [name]]]  ring, 2 s on / 4 s off,
 *                                         with caller ID if a number is given
 *                             call UUID SECS NUMBER|- [NAME...]  ring for an
 *                                         Asterisk call; answer = bridge it
 *                             hangup UUID end that call (caller gave up)
 *                             cidfmt bell|v23|dtmf  caller-ID format
 *                             cidsend number [name]  caller-ID burst only (test)
 *                             talk        line to TALK (PCM audio), tone off
 *                             release     back to dial tone / standby by hook
 *                           state in /var/run/slic.state, the current call
 *                           in /var/run/slic.call ("UUID_phase")
 *
 * Env: SLIC_DEV (default /dev/spidev0.1), SLIC_CS_PER_BYTE, SLIC_TRACE,
 *      SLIC_SPI_HZ, SLIC_RING (A|B), SLIC_DC (A|B), SLIC_TS (timeslot),
 *      SLIC_CID_FMT, SLIC_CID_LEVEL, SLIC_AUDIOSOCKET (listen address,
 *      default 127.0.0.1:4573; "off" disables), SLIC_DIAL_HOOK (default
 *      /usr/libexec/slic-dial), SLIC_DIAL_TIMEOUT (s after the last digit,
 *      default 3).
 *
 * Uses the device, DC, ring and tone profiles extracted verbatim from the
 * stock Tozed mislic binary (stock_profiles.c); the board's buck-boost
 * tracker supply matches none of Microsemi's sample profile sets.
 */
#define _GNU_SOURCE		/* accept4 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <grp.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vp_api.h"
#include "asock.h"
#include "cid.h"
#include "dtmf.h"

extern int SlicHalOpen(const char *path);
extern int SlicHalXfer(const uint8 *tx, int txLen, uint8 *rx, int rxLen);
extern const VpProfileDataType STOCK_DEV_PROFILE[], STOCK_RING_PROFILE_A[],
	STOCK_RING_PROFILE_B[], STOCK_DC_PROFILE_A[], STOCK_DC_PROFILE_B[],
	STOCK_TONE_PROFILE[];

static VpDevCtxType devCtx;
static VpLineCtxType lineCtx;
static Vp886DeviceObjectType devObj;
static Vp886LineObjectType lineObj;
static int offhook;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *cat_name(VpEventCategoryType c)
{
	switch (c) {
	case VP_EVCAT_FAULT: return "FAULT";
	case VP_EVCAT_SIGNALING: return "SIGNALING";
	case VP_EVCAT_RESPONSE: return "RESPONSE";
	case VP_EVCAT_TEST: return "TEST";
	case VP_EVCAT_PROCESS: return "PROCESS";
	default: return "OTHER";
	}
}

/* One pass over the device's pending events (updates offhook);
   returns 1 if (cat,id) was among them. */
static int poll_events(VpEventCategoryType cat, uint16 id)
{
	VpEventType ev;
	int seen = 0;

	devObj.pendingInterrupt = TRUE;	/* simple polled mode */
	while (VpGetEvent(&devCtx, &ev)) {
		/* timestamp rollover fires every ~32 s: housekeeping, not news */
		if (ev.eventCategory == VP_EVCAT_SIGNALING && ev.eventId == VP_DEV_EVID_TS_ROLLOVER)
			continue;
		printf("[%7.3f] %-9s id=0x%04X data=0x%04X ch=%d\n", now(),
		       cat_name(ev.eventCategory), ev.eventId, ev.eventData,
		       ev.channelId);
		if (ev.eventCategory == VP_EVCAT_SIGNALING) {
			if (ev.eventId == VP_LINE_EVID_HOOK_OFF) {
				offhook = 1;
				printf("          -> OFF-HOOK\n");
			} else if (ev.eventId == VP_LINE_EVID_HOOK_ON) {
				offhook = 0;
				printf("          -> ON-HOOK\n");
			}
		}
		if (cat && ev.eventCategory == cat && ev.eventId == id)
			seen = 1;
	}
	return seen;
}

/* Poll the device every 10 ms; return when (cat,id) is seen or timeout. */
static int wait_event(VpEventCategoryType cat, uint16 id, double timeout)
{
	double end = now() + timeout;

	while (now() < end) {
		if (poll_events(cat, id))
			return 0;
		usleep(10000);
	}
	return cat ? -1 : 0;
}

static int cmd_id(void)
{
	uint8 cmd = 0x73, rx[2] = { 0, 0 };

	if (SlicHalXfer(&cmd, 1, rx, 2))
		return 1;
	printf("RCN=0x%02X PCN=0x%02X  ", rx[0], rx[1]);
	if (rx[1] == 0x49)
		printf("-> Le9641 (SF1 low-voltage 1ch tracker). SPI framing OK.\n");
	else if (rx[1] == 0x92 || rx[1] == 0x24 || rx[1] == 0xA4 || rx[1] == 0x93)
		printf("-> looks bit-shifted vs 0x49: mode-0 read timing problem.\n");
	else if (rx[1] == 0xFF || rx[1] == 0x00)
		printf("-> no answer (bus idle). Wrong CS, no clock, or device in reset.\n");
	else
		printf("-> unexpected PCN.\n");
	return rx[1] == 0x49 ? 0 : 2;
}

/* Program PCLK select from the stock profile (0x46 <- 0x02: 2.048 MHz, 8 kHz FS),
   wait, then read the non-unlatching signalling register: bit 7 of byte 0 = CFAIL. */
static int cmd_clk(void)
{
	uint8 devcfg = getenv("SLIC_DEVCFG") ? (uint8)strtol(getenv("SLIC_DEVCFG"), NULL, 0) : 0x02;
	uint8 wr[2] = { 0x46, devcfg }, rd = 0x4D, sig[4] = { 0 };
	int i;

	SlicHalXfer(wr, 2, NULL, 0);
	for (i = 0; i < 5; i++) {
		usleep(20000);
		SlicHalXfer(&rd, 1, sig, 4);
		printf("devcfg=0x%02X sigreg=%02X %02X %02X %02X  CFAIL=%d\n",
		       devcfg, sig[0], sig[1], sig[2], sig[3], !!(sig[0] & 0x80));
	}
	return (sig[0] & 0x80) ? 3 : 0;
}

static int dev_init(void)
{
	VpOptionEventMaskType mask;
	VpProfilePtrType ring = getenv("SLIC_RING") && *getenv("SLIC_RING") == 'A' ?
		STOCK_RING_PROFILE_A : STOCK_RING_PROFILE_B;
	VpProfilePtrType dc = getenv("SLIC_DC") && *getenv("SLIC_DC") == 'A' ?
		STOCK_DC_PROFILE_A : STOCK_DC_PROFILE_B;
	VpStatusType st;

	if (cmd_id() != 0) {
		fprintf(stderr, "refusing to init: device ID check failed\n");
		return -1;
	}
	VpSysInit();
	st = VpMakeDeviceObject(VP_DEV_887_SERIES, 0, &devCtx, &devObj);
	if (st == VP_STATUS_SUCCESS)
		st = VpMakeLineObject(VP_TERM_FXS_GENERIC, 0, &lineCtx, &lineObj, &devCtx);
	if (st != VP_STATUS_SUCCESS) {
		fprintf(stderr, "make objects failed: %d\n", st);
		return -1;
	}
	st = VpInitDevice(&devCtx, STOCK_DEV_PROFILE, VP_PTABLE_NULL, dc, ring,
			  VP_PTABLE_NULL, VP_PTABLE_NULL);
	if (st != VP_STATUS_SUCCESS) {
		fprintf(stderr, "VpInitDevice failed: %d\n", st);
		return -1;
	}
	printf("init started (stock device profile, DC %c, ring %c)\n",
	       dc == STOCK_DC_PROFILE_A ? 'A' : 'B', ring == STOCK_RING_PROFILE_A ? 'A' : 'B');
	if (wait_event(VP_EVCAT_RESPONSE, VP_DEV_EVID_DEV_INIT_CMP, 10)) {
		fprintf(stderr, "no DEV_INIT_CMP within 10 s\n");
		return -1;
	}
	VpMemSet(&mask, 0xFF, sizeof(mask));
	mask.faults = VP_EVCAT_FAULT_UNMASK_ALL;
	mask.signaling = VP_EVCAT_SIGNALING_UNMASK_ALL;
	mask.response = VP_EVCAT_RESPONSE_UNMASK_ALL;
	mask.process = VP_EVCAT_PROCESS_UNMASK_ALL;
	VpSetOption(VP_NULL, &devCtx, VP_OPTION_ID_EVENT_MASK, &mask);

	VpSetLineState(&lineCtx, VP_LINE_STANDBY);
	st = VpCalLine(&lineCtx);
	printf("VpCalLine status %d\n", st);
	if (st == VP_STATUS_SUCCESS && wait_event(VP_EVCAT_RESPONSE, VP_EVID_CAL_CMP, 10))
		fprintf(stderr, "warning: no CAL_CMP within 10 s\n");
	printf("device ready, line STANDBY\n");
	return 0;
}

/* Caller-ID samples for the given format; returns count (0 on bad input). */
static int cid_samples(const char *fmt, const char *num, const char *name, int16_t *buf, int max)
{
	double amp = getenv("SLIC_CID_LEVEL") ? atof(getenv("SLIC_CID_LEVEL")) : 0.25;
	uint8_t msg[64];
	int len;

	if (!strcmp(fmt, "dtmf"))
		return cid_dtmf(buf, max, num, amp);
	len = cid_build_mdmf(msg, sizeof(msg), num, name);
	if (len < 0)
		return 0;
	return cid_fsk(buf, max, msg, len, !strcmp(fmt, "v23"), amp);
}

static int cmd_cidgen(int argc, char **argv)
{
	static int16_t buf[8000 * 3];
	int n;

	if (argc < 4) {
		fprintf(stderr, "usage: slicctl cidgen bell|v23|dtmf NUMBER [NAME] > out.s16\n");
		return 1;
	}
	n = cid_samples(argv[2], argv[3], argc > 4 ? argv[4] : NULL, buf, sizeof(buf) / 2);
	fwrite(buf, 2, n, stdout);
	fprintf(stderr, "%d samples (%.2f s)\n", n, n / 8000.0);
	return n ? 0 : 1;
}

/* ---------------- daemon ----------------
 *
 * One process owns the line: the SLIC (over SPI), its audio (/dev/slicpcm)
 * and the call. Everything runs in one poll() loop with a 10 ms tick, so
 * there are no threads and no locks; the VP-API is only ever called here.
 *
 *   idle      on-hook, line STANDBY
 *   dialtone  off-hook, dial tone, listening for keypad digits
 *   dialing   first digit heard: tone off, collecting until '#' or a pause
 *   ringing   ringing (a "ring" notification or an Asterisk "call")
 *   talk      off-hook with audio: a bridged call, "talk" for slic-play,
 *             or a call being connected
 *   busy      the other side hung up (or the call failed): busy tone
 *
 * Calls reach Asterisk over AudioSocket: Asterisk connects to us
 * (SLIC_AUDIOSOCKET) and sends the call's UUID; we bridge audio only for
 * the UUID of the call in progress. Inbound, the dialplan sends
 * "call UUID ..." and waits for /var/run/slic.call to say UUID_answered.
 * Outbound, we run SLIC_DIAL_HOOK UUID DIGITS, which drops an Asterisk call
 * file whose channel is AudioSocket back to us.
 */
#define CTL_FIFO	"/var/run/slic.ctl"
#define STATE_FILE	"/var/run/slic.state"
#define CALL_FILE	"/var/run/slic.call"
#define PHONE_LED	"/sys/class/leds/blue:phone/brightness"
#define FRAME		160		/* 20 ms at 8 kHz: one AudioSocket frame */
#define TX_MAX_BYTES	1600		/* queue cap toward the phone: 100 ms */

enum dstate { D_IDLE, D_DIALTONE, D_DIALING, D_RINGING, D_TALK, D_BUSY };
static const char *dstate_name[] = { "idle", "dialtone", "dialing", "ringing", "talk", "busy" };
static volatile sig_atomic_t stop_req;

static void on_signal(int sig) { (void)sig; stop_req = 1; }

static void put_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static int pcm = -1;			/* /dev/slicpcm, O_RDWR | O_NONBLOCK */
static int busy_restart;		/* set on entering busy: tone cadence restarts */

/* the call in progress (uuid[0] == 0: none) */
static struct {
	char uuid[40];
	uint8_t bin[16];
	char digits[24];
	double deadline;	/* answered/connecting: AudioSocket must arrive by then */
	pid_t hook;		/* dial hook child, 0 if none */
	int sock;		/* AudioSocket client, -1 if none */
	struct as_rx rx;
	int16_t acc[FRAME];	/* phone audio being gathered into a frame */
	int nacc;
	unsigned drops;		/* frames dropped toward the phone (queue cap) */
} call = { .sock = -1 };

static void call_phase(const char *phase)
{
	char buf[64];

	if (!call.uuid[0])
		return;
	snprintf(buf, sizeof(buf), "%s_%s", call.uuid, phase);
	put_file(CALL_FILE, buf);
	printf("call %s %s\n", call.uuid, phase);
}

/* end the call: tell Asterisk if bridged, record the outcome, forget it */
static void call_end(const char *why)
{
	if (call.sock >= 0) {
		as_send(call.sock, AS_HANGUP, NULL, 0);
		close(call.sock);
		call.sock = -1;
	}
	if (call.uuid[0]) {
		printf("call ended (%s), %u frames dropped\n", why, call.drops);
		call_phase(!strcmp(why, "busy") ? "busy" : "ended");
	}
	call.uuid[0] = 0;
	call.digits[0] = 0;
	call.deadline = 0;
	call.nacc = 0;
	call.drops = 0;
}

static void pcm_flush(int which)
{
	if (pcm >= 0)
		ioctl(pcm, TCFLSH, which);
}

static void set_state(enum dstate *cur, enum dstate next)
{
	char buf[64];

	switch (next) {
	case D_IDLE:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
		break;
	case D_DIALTONE:
		/* TALK (not ACTIVE): the codec must run so we hear the digits */
		VpSetLineState(&lineCtx, VP_LINE_TALK);
		VpSetLineTone(&lineCtx, STOCK_TONE_PROFILE, VP_PTABLE_NULL, VP_NULL);
		pcm_flush(TCIFLUSH);
		break;
	case D_DIALING:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		break;
	case D_RINGING:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_RINGING);
		break;
	case D_TALK:
	case D_BUSY:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_TALK);
		busy_restart = 1;
		break;
	}
	if (*cur != next)
		printf("state %s -> %s (hook %s)\n", dstate_name[*cur], dstate_name[next],
		       offhook ? "off" : "on");
	*cur = next;
	snprintf(buf, sizeof(buf), "state=%s\nhook=%s\n", dstate_name[next], offhook ? "off" : "on");
	put_file(STATE_FILE, buf);
	put_file(PHONE_LED, offhook ? "1" : "0");
	fflush(stdout);
}

/* keep ~60 ms of busy tone queued: Sri Lanka's, 425 Hz 0.75 s on / 0.75 s
   off (ITU E.180 national tones list) */
static void feed_busy_tone(void)
{
	static int t;		/* samples since busy began, mod 12000 (1.5 s) */
	int q = 0, i;
	int16_t s[FRAME];

	if (busy_restart)
		t = 0;
	busy_restart = 0;
	if (ioctl(pcm, TIOCOUTQ, &q) || q > 960)
		return;
	for (i = 0; i < FRAME; i++, t = (t + 1) % 12000)
		s[i] = t < 6000 ? (int16_t)(6000 * sin(2 * M_PI * 425 * t / 8000.0)) : 0;
	if (write(pcm, s, sizeof(s)) < 0 && errno != EAGAIN)
		perror("/dev/slicpcm");
}

/* write a whole buffer through the non-blocking PCM fd (caller-ID bursts) */
static void pcm_write_all(const int16_t *s, int n)
{
	while (n > 0) {
		ssize_t w = write(pcm, s, n * 2);

		if (w > 0) {
			s += w / 2;
			n -= w / 2;
		} else if (w < 0 && errno != EAGAIN) {
			perror("/dev/slicpcm");
			return;
		} else {
			struct pollfd p = { .fd = pcm, .events = POLLOUT };

			poll(&p, 1, 100);
		}
	}
}

static void new_uuid(char *out, size_t len)
{
	FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");

	out[0] = 0;
	if (f) {
		if (fgets(out, len, f))
			out[strcspn(out, "\n")] = 0;
		fclose(f);
	}
}

/* run the dial hook: HOOK UUID DIGITS (no shell; digits are 0-9*#ABCD) */
static pid_t spawn_hook(const char *hook, const char *uuid, const char *digits)
{
	pid_t pid = fork();

	if (pid == 0) {
		execl(hook, hook, uuid, digits, (char *)NULL);
		perror(hook);
		_exit(127);
	}
	return pid;
}

static int cmd_daemon(void)
{
	VpOptionCodecType codec = VP_OPTION_LINEAR;
	VpOptionTimeslotType ts = { 0, 0 };
	enum dstate st = D_IDLE;
	double ring_end = 0, cad_next = 0, last_digit = 0, busy_until = 0, dial_since = 0;
	int ring_on = 0, was_offhook = 0, ctl, dummy, lsock = -1;
	/* caller ID: bell = after the 1st ring; v23/dtmf = before it, after a
	   polarity reversal. Format from SLIC_CID_FMT or the "cidfmt" command. */
	char cid_fmt[8] = "bell", cid_num[24] = "", cid_name[16] = "";
	double cid_at = 0, cid_done_at = 0;
	int cid_after_first = 0, cid_before_ring = 0;
	static int16_t cid_buf[8000 * 3];
	const int16_t *cid_p = NULL;	/* caller-ID samples not yet queued */
	int cid_left = 0;
	const char *as_addr = getenv("SLIC_AUDIOSOCKET") ? getenv("SLIC_AUDIOSOCKET") : "127.0.0.1:4573";
	const char *hook = getenv("SLIC_DIAL_HOOK") ? getenv("SLIC_DIAL_HOOK") : "/usr/libexec/slic-dial";
	double dial_timeout = getenv("SLIC_DIAL_TIMEOUT") ? atof(getenv("SLIC_DIAL_TIMEOUT")) : 3.0;
	struct dtmf_det det;
	struct group *gr;
	char line[256];
	size_t used = 0;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (dev_init())
		return 1;
	VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_CODEC, &codec);
	VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_TIMESLOT, &ts);

	pcm = open("/dev/slicpcm", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (pcm < 0)
		perror("/dev/slicpcm (no audio: dialling and calls disabled)");

	unlink(CTL_FIFO);
	if (mkfifo(CTL_FIFO, 0600) && errno != EEXIST) {
		perror(CTL_FIFO);
		return 1;
	}
	/* Asterisk (user asterisk) sends "call"/"hangup": let its group write */
	if ((gr = getgrnam("asterisk")) && !chown(CTL_FIFO, 0, gr->gr_gid))
		chmod(CTL_FIFO, 0620);
	if (getenv("SLIC_CID_FMT"))
		snprintf(cid_fmt, sizeof(cid_fmt), "%.7s", getenv("SLIC_CID_FMT"));
	ctl = open(CTL_FIFO, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	dummy = open(CTL_FIFO, O_WRONLY | O_CLOEXEC);	/* keeps the FIFO from hitting EOF */
	if (strcmp(as_addr, "off") && pcm >= 0) {
		lsock = as_listen(as_addr);
		if (lsock < 0)
			fprintf(stderr, "AudioSocket: cannot listen on %s: %s\n", as_addr, strerror(errno));
	}
	unlink(CALL_FILE);
	set_state(&st, D_IDLE);
	printf("daemon ready: control via %s, AudioSocket %s\n", CTL_FIFO,
	       lsock >= 0 ? as_addr : "off");
	fflush(stdout);

	while (!stop_req) {
		struct pollfd pf[4];
		int npf = 0, i_ls = -1, i_cs = -1, i_pcm = -1;
		int want_rx = pcm >= 0 && (st == D_DIALTONE || st == D_DIALING ||
					   (call.sock >= 0 && st == D_TALK));
		ssize_t n;
		double t;

		pf[npf++] = (struct pollfd){ .fd = ctl, .events = POLLIN };
		if (lsock >= 0) {
			i_ls = npf;
			pf[npf++] = (struct pollfd){ .fd = lsock, .events = POLLIN };
		}
		if (call.sock >= 0) {
			i_cs = npf;
			pf[npf++] = (struct pollfd){ .fd = call.sock, .events = POLLIN };
		}
		if (want_rx) {
			i_pcm = npf;
			pf[npf++] = (struct pollfd){ .fd = pcm, .events = POLLIN };
		}
		if (poll(pf, npf, 10) < 0 && errno != EINTR)
			break;
		poll_events(0, 0);
		t = now();

		/* reap the dial hook: a failed hook means the call can't happen */
		if (call.hook > 0) {
			int ws;

			if (waitpid(call.hook, &ws, WNOHANG) == call.hook) {
				call.hook = 0;
				if (!WIFEXITED(ws) || WEXITSTATUS(ws)) {
					printf("dial hook failed (status %d)\n", ws);
					call_end("busy");
					if (offhook) {
						set_state(&st, D_BUSY);
						busy_until = t + 30;
					}
				}
			}
		}

		/* hook transitions */
		if (offhook != was_offhook) {
			was_offhook = offhook;
			if (offhook && st == D_RINGING) {
				cid_at = cid_done_at = 0;
				cid_after_first = cid_before_ring = 0;
				cid_left = 0;
				pcm_flush(TCIOFLUSH);
				set_state(&st, D_TALK);		/* answered */
				if (call.uuid[0]) {
					call.deadline = t + 5;	/* Asterisk connects within ms */
					call_phase("answered");
				}
			} else if (offhook && st == D_IDLE) {
				dtmf_init(&det);
				call.digits[0] = 0;
				dial_since = t;
				last_digit = 0;
				set_state(&st, D_DIALTONE);
			} else if (!offhook) {
				call_end("hung up");
				pcm_flush(TCIOFLUSH);
				set_state(&st, D_IDLE);
			} else {
				set_state(&st, st);		/* refresh state file/LED */
			}
		}

		/* phone audio: keypad digits while dialling, call audio when bridged */
		if (i_pcm >= 0 && (pf[i_pcm].revents & POLLIN)) {
			int16_t s[512];

			n = read(pcm, s, sizeof(s));
			if (n > 0 && (st == D_DIALTONE || st == D_DIALING)) {
				char keys[8];
				int k, nk = dtmf_feed(&det, s, n / 2, keys, sizeof(keys));

				for (k = 0; k < nk; k++) {
					size_t dl = strlen(call.digits);

					printf("digit %c\n", keys[k]);
					if (st == D_DIALTONE)
						set_state(&st, D_DIALING);
					last_digit = t;
					if (keys[k] == '#' && dl) {
						last_digit = -1;	/* '#' = dial now */
						break;
					}
					if (dl < sizeof(call.digits) - 1) {
						call.digits[dl] = keys[k];
						call.digits[dl + 1] = 0;
					}
				}
			} else if (n > 0 && call.sock >= 0) {
				int k;

				for (k = 0; k < n / 2; k++) {
					call.acc[call.nacc++] = s[k];
					if (call.nacc == FRAME) {
						/* full socket = Asterisk stalled; drop rather than block */
						as_send(call.sock, AS_AUDIO, call.acc, sizeof(call.acc));
						call.nacc = 0;
					}
				}
			}
		}

		/* end of dialling: '#' or a pause after the last digit */
		if (st == D_DIALING && call.digits[0] &&
		    (last_digit < 0 || t - last_digit >= dial_timeout)) {
			new_uuid(call.uuid, sizeof(call.uuid));
			if (lsock < 0 || as_uuid_parse(call.uuid, call.bin) || access(hook, X_OK)) {
				printf("cannot place a call (no AudioSocket, uuid or %s)\n", hook);
				call_end("busy");
				set_state(&st, D_BUSY);
				busy_until = t + 30;
			} else {
				printf("dialling %s\n", call.digits);
				call_phase("connecting");
				call.deadline = t + 10;
				call.hook = spawn_hook(hook, call.uuid, call.digits);
				pcm_flush(TCIOFLUSH);
				set_state(&st, D_TALK);
			}
		} else if (st == D_DIALING && !call.digits[0] && last_digit < 0) {
			set_state(&st, D_DIALTONE);	/* lone '#': start over */
			last_digit = 0;
		} else if (st == D_DIALTONE && t - dial_since > 30) {
			set_state(&st, D_BUSY);		/* off the hook, never dialled */
			busy_until = t + 30;
		}

		/* AudioSocket: a new connection from Asterisk */
		if (i_ls >= 0 && (pf[i_ls].revents & POLLIN)) {
			int fd = accept4(lsock, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (fd >= 0 && call.sock < 0 && call.uuid[0] && call.deadline) {
				call.sock = fd;
				memset(&call.rx, 0, sizeof(call.rx));
			} else if (fd >= 0) {
				printf("AudioSocket: no call waiting for a connection, refused\n");
				as_send(fd, AS_HANGUP, NULL, 0);
				close(fd);
			}
		}

		/* AudioSocket: frames from Asterisk */
		if (call.sock >= 0 && i_cs >= 0 && (pf[i_cs].revents & (POLLIN | POLLHUP | POLLERR))) {
			int r;

			while ((r = as_recv(call.sock, &call.rx)) == 1) {
				uint8_t kind = call.rx.hdr[0];

				if (kind == AS_UUID) {
					if (call.rx.len != 16 || memcmp(call.rx.buf, call.bin, 16)) {
						printf("AudioSocket: wrong UUID, refused\n");
						r = -1;
						break;
					}
					call.deadline = 0;
					call.nacc = 0;
					pcm_flush(TCIOFLUSH);
					call_phase("bridged");
				} else if (kind == AS_AUDIO && !call.deadline) {
					int q = 0;

					/* bound the delay toward the phone: if the queue is
					   already 100 ms deep (clock drift), drop this frame */
					if (!ioctl(pcm, TIOCOUTQ, &q) && q > TX_MAX_BYTES)
						call.drops++;
					else if (write(pcm, call.rx.buf, call.rx.len & ~1) < 0 && errno != EAGAIN)
						perror("/dev/slicpcm");
				} else if (kind == AS_HANGUP || kind == AS_ERROR) {
					r = -1;
					break;
				}
				/* AS_DTMF from Asterisk: in-band audio already carries it */
			}
			if (r < 0) {
				close(call.sock);
				call.sock = -1;
				call_end("far end hung up");
				pcm_flush(TCOFLUSH);
				if (offhook) {
					set_state(&st, D_BUSY);
					busy_until = t + 30;
				} else {
					set_state(&st, D_IDLE);
				}
			}
		}

		/* Asterisk never connected after answer/dial */
		if (call.uuid[0] && call.deadline && t > call.deadline) {
			printf("AudioSocket: no connection from Asterisk\n");
			call_end("busy");
			if (offhook) {
				set_state(&st, D_BUSY);
				busy_until = t + 30;
			}
		}

		if (st == D_BUSY && pcm >= 0 && t < busy_until)
			feed_busy_tone();

		/* caller-ID samples still to queue (non-blocking) */
		if (cid_left > 0) {
			n = write(pcm, cid_p, cid_left * 2);
			if (n > 0) {
				cid_p += n / 2;
				cid_left -= n / 2;
			}
		}

		/* ring cadence: 2 s on, 4 s off, with the caller-ID burst slotted in */
		if (st == D_RINGING) {
			if (cid_at && t >= cid_at) {
				/* send the burst with the line in on-hook transmission */
				int ns = cid_samples(cid_fmt, cid_num, cid_name[0] ? cid_name : NULL,
						     cid_buf, sizeof(cid_buf) / 2);

				if (!cid_before_ring)
					VpSetLineState(&lineCtx, VP_LINE_OHT);
				if (pcm >= 0 && ns > 0) {
					cid_p = cid_buf;
					cid_left = ns;
				}
				/* the burst plays out from the queue: allow for all of it
				   plus a margin before the next ring */
				cid_done_at = t + ns / 8000.0 + 0.3;
				printf("caller ID %s '%s' sent (%.2f s)\n", cid_fmt, cid_num, ns / 8000.0);
				fflush(stdout);
				cid_at = 0;
			} else if (cid_done_at && t >= cid_done_at) {
				cid_done_at = 0;
				VpSetLineState(&lineCtx, VP_LINE_STANDBY);	/* normal polarity */
				if (cid_before_ring) {			/* now start ringing */
					cid_before_ring = 0;
					ring_on = 1;
					VpSetLineState(&lineCtx, VP_LINE_RINGING);
					cad_next = now() + 2.0;
				}
			} else if (t >= ring_end) {
				call_end("no answer");
				set_state(&st, offhook ? D_DIALTONE : D_IDLE);
			} else if (!cid_at && !cid_done_at && !cid_before_ring && t >= cad_next) {
				ring_on = !ring_on;
				VpSetLineState(&lineCtx, ring_on ? VP_LINE_RINGING : VP_LINE_STANDBY);
				cad_next = t + (ring_on ? 2.0 : 4.0);
				if (!ring_on && cid_after_first) {
					cid_after_first = 0;
					cid_at = t + 0.6;	/* GR-30: >= 500 ms after the ring */
				}
			}
		}

		/* control commands, one per line */
		n = read(ctl, line + used, sizeof(line) - 1 - used);
		if (n > 0) {
			char *nl;

			used += n;
			line[used] = 0;
			while ((nl = strchr(line, '\n'))) {
				char cmd[16] = "", num[24] = "", name[16] = "", word[40] = "";
				double arg = 0;
				int is_call;

				*nl = 0;
				sscanf(line, "%15s %lf %23s %15s", cmd, &arg, num, name);
				sscanf(line, "%*s %39s", word);
				printf("command: %s\n", line);
				is_call = !strcmp(cmd, "call");
				if (is_call) {
					/* call UUID SECS NUMBER|- [NAME words...] */
					int off = 0;

					num[0] = name[0] = 0;
					arg = 0;
					sscanf(line, "%*s %*s %lf %23s %n", &arg, num, &off);
					if (off)
						snprintf(name, sizeof(name), "%s", line + off);
					if (!strcmp(num, "-"))
						num[0] = 0;
					if (st != D_IDLE || offhook || call.uuid[0] ||
					    as_uuid_parse(word, call.bin)) {
						/* report busy for this UUID without touching ours */
						char buf[64];

						snprintf(buf, sizeof(buf), "%s_busy", word);
						if (!call.uuid[0])
							put_file(CALL_FILE, buf);
						printf("call %s refused: line busy\n", word);
						goto next;
					}
					snprintf(call.uuid, sizeof(call.uuid), "%s", word);
					call_phase("ringing");
				}
				if ((is_call || !strcmp(cmd, "ring")) && !offhook && st == D_IDLE) {
					/* ring SECS [NUMBER [NAME]] */
					snprintf(cid_num, sizeof(cid_num), "%s", num);
					snprintf(cid_name, sizeof(cid_name), "%s", name);
					cid_at = cid_done_at = 0;
					cid_after_first = cid_before_ring = 0;
					ring_end = now() + (arg > 0 ? arg : 30);
					ring_on = 1;
					cad_next = now() + 2.0;
					set_state(&st, D_RINGING);
					if (cid_num[0] && !strcmp(cid_fmt, "bell")) {
						cid_after_first = 1;		/* between 1st and 2nd ring */
					} else if (cid_num[0]) {
						/* ETSI/DTMF: reverse polarity, send, then ring */
						cid_before_ring = 1;
						ring_on = 0;
						VpSetLineState(&lineCtx, VP_LINE_OHT_POLREV);
						cid_at = now() + 0.3;
					}
				} else if (!strcmp(cmd, "hangup")) {
					/* the caller gave up: only our call, in any phase */
					if (call.uuid[0] && !strcmp(word, call.uuid)) {
						int ringing = st == D_RINGING;

						call_end("caller hung up");
						cid_left = 0;
						if (ringing)
							set_state(&st, offhook ? D_DIALTONE : D_IDLE);
						else if (offhook) {
							set_state(&st, D_BUSY);
							busy_until = t + 30;
						}
					}
				} else if (!strcmp(cmd, "cidsend") && !offhook && st == D_IDLE) {
					/* cidsend NUMBER [NAME]: burst only, no ringing (testing) */
					int ns;

					sscanf(line, "%*s %23s %15s", num, name);
					ns = cid_samples(cid_fmt, num, name[0] ? name : NULL,
							 cid_buf, sizeof(cid_buf) / 2);
					VpSetLineState(&lineCtx, strcmp(cid_fmt, "bell") ?
						       VP_LINE_OHT_POLREV : VP_LINE_OHT);
					usleep(300000);
					if (pcm >= 0 && ns > 0)
						pcm_write_all(cid_buf, ns);
					usleep(800000);		/* last 0.5 s still queued + margin */
					VpSetLineState(&lineCtx, VP_LINE_STANDBY);
					printf("caller ID %s '%s' sent without ringing\n", cid_fmt, num);
				} else if (!strcmp(cmd, "cidfmt")) {
					if (!strcmp(word, "bell") || !strcmp(word, "v23") || !strcmp(word, "dtmf"))
						snprintf(cid_fmt, sizeof(cid_fmt), "%.7s", word);
					printf("caller ID format: %s\n", cid_fmt);
				} else if (!strcmp(cmd, "talk") && !call.uuid[0]) {
					set_state(&st, D_TALK);
				} else if ((!strcmp(cmd, "release") || !strcmp(cmd, "stop")) && !call.uuid[0]) {
					set_state(&st, offhook ? D_DIALTONE : D_IDLE);
					if (offhook) {
						dtmf_init(&det);
						dial_since = t;
						last_digit = 0;
					}
				}
next:
				memmove(line, nl + 1, strlen(nl + 1) + 1);
				used = strlen(line);
			}
			if (used >= sizeof(line) - 1)
				used = 0;	/* overlong garbage: drop */
		}
		fflush(stdout);
	}
	call_end("daemon stopping");
	set_state(&st, D_IDLE);
	put_file(PHONE_LED, "0");
	if (lsock >= 0)
		close(lsock);
	if (pcm >= 0)
		close(pcm);
	close(ctl);
	close(dummy);
	unlink(CTL_FIFO);
	unlink(STATE_FILE);
	unlink(CALL_FILE);
	printf("daemon stopped, line STANDBY\n");
	return 0;
}

/* offline test: keypad digits in s16 8 kHz audio on stdin */
static int cmd_dtmfdet(void)
{
	struct dtmf_det det;
	int16_t s[1024];
	char keys[16];
	size_t n;
	long total = 0;

	dtmf_init(&det);
	while ((n = fread(s, 2, 1024, stdin)) > 0) {
		int k, nk = dtmf_feed(&det, s, n, keys, sizeof(keys));

		for (k = 0; k < nk; k++)
			printf("%c", keys[k]);
		total += n;
	}
	printf("\n");
	fprintf(stderr, "%.2f s of audio\n", total / 8000.0);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = getenv("SLIC_DEV") ? getenv("SLIC_DEV") : "/dev/spidev0.1";
	const char *cmd = argc > 1 ? argv[1] : "id";
	double secs = argc > 2 ? atof(argv[2]) : 5;

	if (!strcmp(cmd, "cidgen"))
		return cmd_cidgen(argc, argv);	/* no hardware needed */
	if (!strcmp(cmd, "dtmfdet"))
		return cmd_dtmfdet();		/* no hardware needed */

	if (SlicHalOpen(dev))
		return 1;
	if (!strcmp(cmd, "id"))
		return cmd_id();
	if (!strcmp(cmd, "clk"))
		return cmd_clk();
	if (!strcmp(cmd, "daemon"))
		return cmd_daemon();
	if (dev_init())
		return 1;

	if (!strcmp(cmd, "ring")) {
		printf("RINGING for %.0f s (lift the handset to stop early)\n", secs);
		VpSetLineState(&lineCtx, VP_LINE_RINGING);
		wait_event(0, 0, secs);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
	} else if (!strcmp(cmd, "tone")) {
		printf("ACTIVE + stock tone profile for %.0f s (lift the handset)\n", secs);
		VpSetLineState(&lineCtx, VP_LINE_ACTIVE);
		VpSetLineTone(&lineCtx, STOCK_TONE_PROFILE, VP_PTABLE_NULL, VP_NULL);
		{
			double t0 = now();
			uint8 rd = 0x4D, sig[4];

			while (now() - t0 < secs) {
				wait_event(0, 0, 1.0);
				SlicHalXfer(&rd, 1, sig, 4);	/* non-unlatching read */
				printf("t=%4.0fs raw sigreg %02X %02X %02X %02X  hook=%d\n",
				       now() - t0, sig[0], sig[1], sig[2], sig[3], sig[0] & 0x01);
				fflush(stdout);
			}
		}
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
	} else if (!strcmp(cmd, "monitor")) {
		double end = now() + secs;
		int was = 0;

		printf("monitoring hook for %.0f s\n", secs);
		while (now() < end) {
			wait_event(0, 0, 0.05);
			if (offhook != was) {
				was = offhook;
				VpSetLineState(&lineCtx, offhook ? VP_LINE_ACTIVE : VP_LINE_STANDBY);
				VpSetLineTone(&lineCtx, offhook ? STOCK_TONE_PROFILE : VP_PTABLE_NULL,
					      VP_PTABLE_NULL, VP_NULL);
			}
		}
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
	} else if (!strcmp(cmd, "talk")) {
		VpOptionCodecType codec = VP_OPTION_LINEAR;
		VpOptionTimeslotType ts;

		ts.tx = ts.rx = getenv("SLIC_TS") ? (uint8)atoi(getenv("SLIC_TS")) : 0;
		VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_CODEC, &codec);
		VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_TIMESLOT, &ts);
		VpSetLineState(&lineCtx, VP_LINE_TALK);
		/* SLIC_TALK_TONE=1: also run the internal tone generator. Its echo
		   through the line hybrid shows up on the PCM transmit side, which
		   gives a loopback signal for tuning alignment without a listener. */
		if (getenv("SLIC_TALK_TONE") && atoi(getenv("SLIC_TALK_TONE")))
			VpSetLineTone(&lineCtx, STOCK_TONE_PROFILE, VP_PTABLE_NULL, VP_NULL);
		printf("TALK, linear PCM timeslot %u, for %.0f s: write audio to /dev/slicpcm\n",
		       ts.tx, secs);
		fflush(stdout);
		wait_event(0, 0, secs);
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
	} else {
		fprintf(stderr, "unknown command %s\n", cmd);
		return 1;
	}
	printf("done, line STANDBY\n");
	return 0;
}
