// What api->sys->getFirmwareId returns: the revision this build came from, generated for every
// build by cmake/picodeck_firmware_id.cmake, so unlike __DATE__ it does not go stale after an
// incremental one. The header holds only the id, so this file is all a new commit recompiles.
//
// A fixed size, so that going from clean to dirty moves nothing after it in flash: layout alone
// moves whole runs on the device by 2-3%.
#include "firmware_id.h"
#include "os.h"

static const char s_firmware_id[48] = PICODECK_FIRMWARE_ID;

const char *firmware_id(void) { return s_firmware_id; }
