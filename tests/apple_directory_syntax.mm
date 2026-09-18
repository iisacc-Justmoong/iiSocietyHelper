#include <QtGlobal>
#ifndef Q_OS_IOS
#error This compile check must exercise the iOS branch of the App Group resolver.
#endif
#include "../src/platform/apple/ObservationDirectory.mm"
