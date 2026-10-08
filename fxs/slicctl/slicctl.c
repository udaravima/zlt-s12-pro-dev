/*
 * slicctl - bring-up tool for the Le9641 SLIC on the ZLT S12 Pro.
 *
 *   slicctl id              raw MPI read of RCN/PCN (expects PCN 0x49)
 *   slicctl clk             set PCLK select, report the CFAIL (clock fault) bit
 *   slicctl ring [sec]      init + calibrate, ring for sec seconds
 *   slicctl tone [sec]      init, line ACTIVE, internal tone generator on
 *   slicctl monitor [sec]   print hook events; tone while off-hook
 *   slicctl talk [sec]      init, line TALK, 16-bit linear PCM on timeslot 0
 *   slicctl daemon          keep the line ready: dial tone while off-hook,
 *                           commands on the FIFO /var/run/slic.ctl:
 *                             ring [sec]  ring with 2 s on / 4 s off cadence
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
			if (offhook && st == D_RINGING)
				set_state(&st, D_TALK);		/* answered */
			else if (offhook && st == D_IDLE)
				set_state(&st, D_DIALTONE);
			else if (!offhook)
				set_state(&st, D_IDLE);
			else
				set_state(&st, st);		/* refresh state file/LED */
		}

		/* ring cadence: 2 s on, 4 s off */
		if (st == D_RINGING) {
			double t = now();

			if (t >= ring_end) {
				set_state(&st, offhook ? D_DIALTONE : D_IDLE);
			} else if (t >= cad_next) {
				ring_on = !ring_on;
				VpSetLineState(&lineCtx, ring_on ? VP_LINE_RINGING : VP_LINE_STANDBY);
				cad_next = t + (ring_on ? 2.0 : 4.0);
			}
		}

		/* control commands, one per line */
		n = read(ctl, line + used, sizeof(line) - 1 - used);
		if (n > 0) {
			char *nl;

			used += n;
			line[used] = 0;
			while ((nl = strchr(line, '\n'))) {
				char cmd[16] = "";
				double arg = 0;

				*nl = 0;
				sscanf(line, "%15s %lf", cmd, &arg);
				printf("command: %s\n", line);
				if (!strcmp(cmd, "ring") && !offhook) {
					ring_end = now() + (arg > 0 ? arg : 30);
					ring_on = 1;
					cad_next = now() + 2.0;
					set_state(&st, D_RINGING);
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
