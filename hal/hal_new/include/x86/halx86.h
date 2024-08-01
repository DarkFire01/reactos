#pragma once

/* Internal HAL Headers */
#include "bus.h"
#include "halirq.h"
#include "haldma.h"
#if defined(SARCH_PC98)
#include <drivers/pc98/cpu.h>
#include <drivers/pc98/pic.h>
#include <drivers/pc98/pit.h>
#include <drivers/pc98/rtc.h>
#include <drivers/pc98/sysport.h>
#include <drivers/pc98/video.h>
#else
#include "halhw.h"
#endif
#include "halp.h"
#include "mps.h"
#include "halacpi.h"