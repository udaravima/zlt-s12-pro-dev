/*
 * System services for VP-API-II on Linux userspace (single-threaded slicctl).
 * Critical sections only need nesting bookkeeping: one thread owns the device.
 */
#include <unistd.h>
#include "vp_api_types.h"
#include "sys_service.h"

static int nesting[VP_NUM_CRITICAL_SEC_TYPES];

bool VpSysInit(void) { return TRUE; }

void VpSysServiceToggleLed(uint8 ledNum) { (void)ledNum; }

uint8 VpSysEnterCritical(VpDeviceIdType deviceId, VpCriticalSecType t)
{
	(void)deviceId;
	return (uint8)++nesting[t];
}

uint8 VpSysExitCritical(VpDeviceIdType deviceId, VpCriticalSecType t)
{
	(void)deviceId;
	if (nesting[t] > 0)
		nesting[t]--;
	return (uint8)nesting[t];
}

/* time is in 125 us units */
void VpSysWait(uint8 time) { usleep(125u * time); }

void VpSysDisableInt(VpDeviceIdType deviceId) { (void)deviceId; }
void VpSysEnableInt(VpDeviceIdType deviceId) { (void)deviceId; }
bool VpSysTestInt(VpDeviceIdType deviceId) { (void)deviceId; return TRUE; }

void VpSysDtmfDetEnable(VpDeviceIdType d, uint8 c) { (void)d; (void)c; }
void VpSysDtmfDetDisable(VpDeviceIdType d, uint8 c) { (void)d; (void)c; }

void *VpSysTestHeapAcquire(uint8 *pHeapId) { (void)pHeapId; return VP_NULL; }
bool VpSysTestHeapRelease(uint8 heapId) { (void)heapId; return FALSE; }

void VpSysPcmCollectAndProcess(void *pLineCtx, VpDeviceIdType deviceId,
	uint8 channelId, uint8 startTimeslot, uint16 operationTime,
	uint16 settlingTime, uint16 operationMask)
{
	(void)pLineCtx; (void)deviceId; (void)channelId; (void)startTimeslot;
	(void)operationTime; (void)settlingTime; (void)operationMask;
}
