// Compiles the vendored QOA reference implementation (qoa.h, MIT).
// Firmware defines QOA_NO_ENCODER (decode only); host tools and unit tests
// leave it undefined so fixtures can be encoded in-process.
#define QOA_NO_STDIO
#define QOA_IMPLEMENTATION
#include "qoa.h"
