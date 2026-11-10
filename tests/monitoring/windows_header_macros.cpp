#include <algorithm>
#include <limits>
#include <windows.h>

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#include <iiSocietyHelper/Monitoring/Monitor.h>

#if !defined(min) || !defined(max)
#error "iiSocietyHelper public headers must preserve the consuming application's macros"
#endif
