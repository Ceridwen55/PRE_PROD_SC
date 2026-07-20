// src/log.cpp -- the single definition of LOG (see log.h for the "why").

#include "log.h"

#if SERIAL_DEBUG
  Print& LOG = Serial;   // forward to the real USB-CDC Serial
#else
  NullSerial LOG;        // discard everything
#endif
