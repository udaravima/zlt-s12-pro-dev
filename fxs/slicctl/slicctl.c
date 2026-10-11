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
 *   slicctl daemon          keep the line ready: dial tone while off-hook,
 *                           commands on the FIFO /var/run/slic.ctl:
 *                             ring [sec [number [name]]]  ring, 2 s on / 4 s off,
 *                                         with caller ID if a number is given
 *                             cidfmt bell|v23|dtmf  caller-ID format
 *                             cidsend number [name]  caller-ID burst only (test)
 *                             talk        line to TALK (PCM audio), tone off
 *                             release     back to dial tone / standby by hook
 *                           state in /var/run/slic.state
 *
 * Env: SLIC_DEV (default /dev/spidev0.1), SLIC_CS_PER_BYTE, SLIC_TRACE,
 *      SLIC_SPI_HZ, SLIC_RING (A|B), SLIC_DC (A|B), SLIC_TS (timeslot).
 *
 * Uses the device, DC, ring and tone profiles extracted verbatim from the
 * stock Tozed mislic binary (stock_profiles.c); the board's buck-boost
 * tracker supply matches none of Microsemi's sample profile sets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vp_api.h"
#include "cid.h"

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

/* Poll the device every 10 ms; return when (cat,id) is seen or timeout. */
static int wait_event(VpEventCategoryType cat, uint16 id, double timeout)
{
	double end = now() + timeout;
	VpEventType ev;

	while (now() < end) {
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
			if (ev.eventCategory == cat && ev.eventId == id)
				return 0;
		}
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

/* ---------------- daemon ---------------- */

#define CTL_FIFO	"/var/run/slic.ctl"
#define STATE_FILE	"/var/run/slic.state"
#define PHONE_LED	"/sys/class/leds/blue:phone/brightness"

enum dstate { D_IDLE, D_DIALTONE, D_RINGING, D_TALK };
static const char *dstate_name[] = { "idle", "dialtone", "ringing", "talk" };
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

static void set_state(enum dstate *cur, enum dstate next)
{
	char buf[64];

	switch (next) {
	case D_IDLE:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_STANDBY);
		break;
	case D_DIALTONE:
		VpSetLineState(&lineCtx, VP_LINE_ACTIVE);
		VpSetLineTone(&lineCtx, STOCK_TONE_PROFILE, VP_PTABLE_NULL, VP_NULL);
		break;
	case D_RINGING:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_RINGING);
		break;
	case D_TALK:
		VpSetLineTone(&lineCtx, VP_PTABLE_NULL, VP_PTABLE_NULL, VP_NULL);
		VpSetLineState(&lineCtx, VP_LINE_TALK);
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

static int cmd_daemon(void)
{
	VpOptionCodecType codec = VP_OPTION_LINEAR;
	VpOptionTimeslotType ts = { 0, 0 };
	enum dstate st = D_IDLE;
	double ring_end = 0, cad_next = 0;
	int ring_on = 0, was_offhook = 0, ctl, dummy;
	/* caller ID: bell = after the 1st ring; v23/dtmf = before it, after a
	   polarity reversal. Format from SLIC_CID_FMT or the "cidfmt" command. */
	char cid_fmt[8] = "bell", cid_num[24] = "", cid_name[16] = "";
	double cid_at = 0, cid_done_at = 0;
	int cid_after_first = 0, cid_before_ring = 0, pcm = -1;
	static int16_t cid_buf[8000 * 3];
	char line[128];
	size_t used = 0;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (dev_init())
		return 1;
	VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_CODEC, &codec);
	VpSetOption(&lineCtx, VP_NULL, VP_OPTION_ID_TIMESLOT, &ts);

	unlink(CTL_FIFO);
	if (mkfifo(CTL_FIFO, 0600) && errno != EEXIST) {
		perror(CTL_FIFO);
		return 1;
	}
	if (getenv("SLIC_CID_FMT"))
		snprintf(cid_fmt, sizeof(cid_fmt), "%.7s", getenv("SLIC_CID_FMT"));
	ctl = open(CTL_FIFO, O_RDONLY | O_NONBLOCK);
	dummy = open(CTL_FIFO, O_WRONLY);	/* keeps the FIFO from hitting EOF */
	set_state(&st, D_IDLE);
	printf("daemon ready: control via %s\n", CTL_FIFO);
	fflush(stdout);

	while (!stop_req) {
		ssize_t n;

		wait_event(0, 0, 0.02);

		/* hook transitions */
		if (offhook != was_offhook) {
			was_offhook = offhook;
			if (offhook && st == D_RINGING) {
				cid_at = cid_done_at = 0;
				cid_after_first = cid_before_ring = 0;
				set_state(&st, D_TALK);		/* answered */
			}
			else if (offhook && st == D_IDLE)
				set_state(&st, D_DIALTONE);
			else if (!offhook)
				set_state(&st, D_IDLE);
			else
				set_state(&st, st);		/* refresh state file/LED */
		}

		/* ring cadence: 2 s on, 4 s off, with the caller-ID burst slotted in */
		if (st == D_RINGING) {
			double t = now();

			if (cid_at && t >= cid_at) {
				/* send the burst with the line in on-hook transmission */
				int ns = cid_samples(cid_fmt, cid_num, cid_name[0] ? cid_name : NULL,
						     cid_buf, sizeof(cid_buf) / 2);

				if (!cid_before_ring)
					VpSetLineState(&lineCtx, VP_LINE_OHT);
				if (pcm < 0)
					pcm = open("/dev/slicpcm", O_WRONLY);
				if (pcm >= 0 && ns > 0 && write(pcm, cid_buf, ns * 2) < 0)
					perror("/dev/slicpcm");
				/* write() returns once queued (<= 0.5 s ahead): allow for the
				   rest of the burst plus a margin before the next ring */
				cid_done_at = now() + ns / 8000.0 + 0.3;
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
				char cmd[16] = "", num[24] = "", name[16] = "", word[16] = "";
				double arg = 0;

				*nl = 0;
				sscanf(line, "%15s %lf %23s %15s", cmd, &arg, num, name);
				sscanf(line, "%*s %15s", word);
				printf("command: %s\n", line);
				if (!strcmp(cmd, "ring") && !offhook) {
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
				} else if (!strcmp(cmd, "cidsend") && !offhook && st == D_IDLE) {
					/* cidsend NUMBER [NAME]: burst only, no ringing (testing) */
					int ns;

					sscanf(line, "%*s %23s %15s", num, name);
					ns = cid_samples(cid_fmt, num, name[0] ? name : NULL,
							 cid_buf, sizeof(cid_buf) / 2);
					VpSetLineState(&lineCtx, strcmp(cid_fmt, "bell") ?
						       VP_LINE_OHT_POLREV : VP_LINE_OHT);
					usleep(300000);
					if (pcm < 0)
						pcm = open("/dev/slicpcm", O_WRONLY);
					if (pcm >= 0 && ns > 0 && write(pcm, cid_buf, ns * 2) < 0)
						perror("/dev/slicpcm");
					usleep((useconds_t)(ns / 8000.0 * 1e6) + 300000);
					VpSetLineState(&lineCtx, VP_LINE_STANDBY);
					printf("caller ID %s '%s' sent without ringing\n", cid_fmt, num);
				} else if (!strcmp(cmd, "cidfmt")) {
					if (!strcmp(word, "bell") || !strcmp(word, "v23") || !strcmp(word, "dtmf"))
						snprintf(cid_fmt, sizeof(cid_fmt), "%.7s", word);
					printf("caller ID format: %s\n", cid_fmt);
				} else if (!strcmp(cmd, "talk")) {
					set_state(&st, D_TALK);
				} else if (!strcmp(cmd, "release") || !strcmp(cmd, "stop")) {
					set_state(&st, offhook ? D_DIALTONE : D_IDLE);
				}
				memmove(line, nl + 1, strlen(nl + 1) + 1);
				used = strlen(line);
			}
			if (used >= sizeof(line) - 1)
				used = 0;	/* overlong garbage: drop */
		}
	}
	set_state(&st, D_IDLE);
	put_file(PHONE_LED, "0");
	if (pcm >= 0)
		close(pcm);
	close(ctl);
	close(dummy);
	unlink(CTL_FIFO);
	unlink(STATE_FILE);
	printf("daemon stopped, line STANDBY\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = getenv("SLIC_DEV") ? getenv("SLIC_DEV") : "/dev/spidev0.1";
	const char *cmd = argc > 1 ? argv[1] : "id";
	double secs = argc > 2 ? atof(argv[2]) : 5;

	if (!strcmp(cmd, "cidgen"))
		return cmd_cidgen(argc, argv);	/* no hardware needed */

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
