/* Device profile of the ZLT S12 Pro's Le9641 SLIC board.
 *
 * NOT covered by this repository's licence (GPL-2.0): these bytes are data
 * from the router's stock firmware (Tozed), not our work, and no licence to
 * them is granted here. They are included only so the phone port can work
 * (interoperability) on routers whose stock firmware is gone.
 *
 * VP-API-II Profile Wizard format: 0x0D (VP886/887 family), 0xFF (device
 * profile), index 0, 0x33 bytes follow, format version 8. It configures the
 * PCM clock and the SLIC's own switching supply (a buck-boost tracker that
 * makes the battery and ringing voltages), which depends on the parts on
 * this board: Microsemi's published sample sets cover other supply types
 * (flyback, inverting boost) and an older format, so none of them can stand
 * in for it.
 *
 * The bytes come verbatim from the stock firmware's mislic daemon
 * (fxs/tools/extract_profiles.py on a stock mtd4 dump prints the same
 * table; SHA-256 of the 55 bytes:
 * d78fe651b81f8de200a90e5dd4392ab0917d94c82ead459cc520bd9314997bcf).
 * The DC-feed and ringing profiles are not needed here: VP-API's built-in
 * defaults are byte-identical to the copies in that binary.
 */
#include "vp_api_types.h"

const VpProfileDataType BOARD_DEV_PROFILE[] = {
    0x0D, 0xFF, 0x00, 0x33, 0x08, 0x14, 0x46, 0x02, 0x44, 0x00, 0x5E, 0x14,
    0x00, 0xF6, 0x95, 0x00, 0x58, 0x30, 0x5C, 0x30, 0xE4, 0x04, 0x8F, 0x0A,
    0xE6, 0x60, 0x02, 0xA0, 0x00, 0x00, 0x01, 0x30, 0x14, 0x30, 0x14, 0x30,
    0x14, 0xFF, 0x95, 0x00, 0x62, 0x62, 0x04, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1B, 0xFF, 0x14, 0x00, 0x00,
};
