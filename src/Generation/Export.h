#pragma once

#if defined(_WIN32)
#  if defined(IISOCIETYHELPER_GENERATION_BUILD)
#    define IISOCIETYHELPER_GENERATION_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETYHELPER_GENERATION_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETYHELPER_GENERATION_EXPORT __attribute__((visibility("default")))
#endif
