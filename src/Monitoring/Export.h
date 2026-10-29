#pragma once
#if defined(IISOCIETYHELPER_MONITORING_STATIC)
# define IISOCIETYHELPER_MONITORING_EXPORT
#elif defined(_WIN32)
# if defined(IISOCIETYHELPER_MONITORING_BUILDING_LIBRARY)
#  define IISOCIETYHELPER_MONITORING_EXPORT __declspec(dllexport)
# else
#  define IISOCIETYHELPER_MONITORING_EXPORT __declspec(dllimport)
# endif
#elif defined(__GNUC__) || defined(__clang__)
# define IISOCIETYHELPER_MONITORING_EXPORT __attribute__((visibility("default")))
#else
# define IISOCIETYHELPER_MONITORING_EXPORT
#endif
