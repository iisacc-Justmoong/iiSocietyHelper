#pragma once

#include <QString>
#include <QtGlobal>

#if defined(IISOCIETYHELPER_BUILDING_LIBRARY)
#  define IISOCIETYHELPER_EXPORT Q_DECL_EXPORT
#else
#  define IISOCIETYHELPER_EXPORT Q_DECL_IMPORT
#endif

namespace iiSocietyHelper {

[[nodiscard]] IISOCIETYHELPER_EXPORT QString helloWorld();

} // namespace iiSocietyHelper
