// Compiles the vendored QOA reference implementation (qoa.h, MIT) for the
// host unit tests: its encoder makes fixtures and its decoder is the oracle
// for src/drivers/qoa.c's streaming decoder.  The firmware and simulator
// do not build it.
#define QOA_NO_STDIO
#define QOA_IMPLEMENTATION
#include "qoa.h"
