/*
 * VP-API-II MPI HAL over Linux spidev (/dev/spidev0.1 on the ZLT S12 Pro).
 *
 * MPI framing: [EC write 0x4A, ecVal] (skipped when reading EC), command byte,
 * then cmdLen data bytes; odd command = read. The MT7621 SPI controller is
 * half-duplex, so reads are rx-only transfers.
 *
 * ZL880/miSLIC accept either 2.5 us chip-select off-time between bytes
 * ("MPI" mode, SLIC_CS_PER_BYTE=1) or CS held for the whole command
 * (default; Microsemi "SPI8 CFM" mode). Verified both read PCN 0x49.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include "vp_api_types.h"
#include "mpi_hal.h"
#include "sys_service.h"

#define CSLAC_EC_REG_RD		0x4B
#define CSLAC_EC_REG_WRT	0x4A
#define MAX_XFER		260	/* 3 header bytes + up to 255 data bytes */

static int spi_fd = -1;
static int cs_per_byte;	/* held CS is the default: per-byte CS overflows spidev bufsiz on long API bursts */
static int trace;
static unsigned int speed_hz;

int SlicHalOpen(const char *path)
{
	const char *e;
	uint8_t mode = SPI_MODE_0, bits = 8;

	spi_fd = open(path, O_RDWR);
	if (spi_fd < 0) {
		perror(path);
		return -1;
	}
	e = getenv("SLIC_CS_PER_BYTE");
	if (e)
		cs_per_byte = atoi(e);
	e = getenv("SLIC_TRACE");
	if (e)
		trace = atoi(e);
	e = getenv("SLIC_SPI_HZ");
	speed_hz = e ? (unsigned int)atoi(e) : 0;
	if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
		perror("spidev setup");
		return -1;
	}
	return 0;
}

/* Send tx bytes then receive rxLen bytes, one SPI message. */
int SlicHalXfer(const uint8 *tx, int txLen, uint8 *rx, int rxLen)
{
	struct spi_ioc_transfer x[MAX_XFER];
	int n = 0, i;

	memset(x, 0, sizeof(x));
	if (txLen + rxLen > MAX_XFER)
		return -1;
	if (cs_per_byte) {
		for (i = 0; i < txLen; i++, n++) {
			x[n].tx_buf = (unsigned long)&tx[i];
			x[n].len = 1;
		}
		for (i = 0; i < rxLen; i++, n++) {
			x[n].rx_buf = (unsigned long)&rx[i];
			x[n].len = 1;
		}
		for (i = 0; i < n - 1; i++) {
			x[i].cs_change = 1;	/* release CS between bytes */
			x[i].delay_usecs = 3;	/* >= 2.5 us off-time */
		}
	} else {
		if (txLen) {
			x[n].tx_buf = (unsigned long)tx;
			x[n++].len = txLen;
		}
		if (rxLen) {
			x[n].rx_buf = (unsigned long)rx;
			x[n++].len = rxLen;
		}
	}
	for (i = 0; i < n; i++) {
		x[i].bits_per_word = 8;
		x[i].speed_hz = speed_hz;
	}
	if (ioctl(spi_fd, SPI_IOC_MESSAGE(n), x) < 0) {
		perror("SPI_IOC_MESSAGE");
		return -1;
	}
	return 0;
}

void VpMpiCmd(VpDeviceIdType deviceId, uint8 ecVal, uint8 cmd,
	      uint8 cmdLen, uint8 *dataPtr)
{
	uint8 hdr[3];
	int h = 0, i;

	(void)deviceId;
	VpSysEnterCritical(deviceId, VP_MPI_CRITICAL_SEC);
	if (cmd != CSLAC_EC_REG_RD) {
		hdr[h++] = CSLAC_EC_REG_WRT;
		hdr[h++] = ecVal;
	}
	hdr[h++] = cmd;

	if (cmd & 1) {
		SlicHalXfer(hdr, h, dataPtr, cmdLen);
	} else {
		uint8 buf[MAX_XFER];

		memcpy(buf, hdr, h);
		memcpy(buf + h, dataPtr, cmdLen);
		SlicHalXfer(buf, h + cmdLen, NULL, 0);
	}
	if (trace) {
		fprintf(stderr, "MPI %s ec=%02X cmd=%02X:", (cmd & 1) ? "RD" : "WR", ecVal, cmd);
		for (i = 0; i < cmdLen; i++)
			fprintf(stderr, " %02X", dataPtr[i]);
		fputc('\n', stderr);
	}
	VpSysExitCritical(deviceId, VP_MPI_CRITICAL_SEC);
}
