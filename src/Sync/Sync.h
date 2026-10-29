#pragma once

#include <QString>
#include "iiSocietyHelper/Export.h"

namespace iiSocietyHelper::Sync {

[[nodiscard]] IISOCIETYHELPER_EXPORT QString helloWorld();

} // namespace iiSocietyHelper::Sync

#include "Replica.h"
#include "Synchronizer.h"
#include "Controller.h"
#include "RemoteFiles.h"
#include "NearbyBootstrap.h"
#include "BleDiscovery.h"
#include "ObjectProvider.h"
