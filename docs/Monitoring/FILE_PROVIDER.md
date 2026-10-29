# File creation ownership in iiSocietyHelper::Monitoring

Process ownership files and atomic observation output use native provider/Core file channels. Monitoring, observation and cleanup decisions remain in Monitor.

The common [ownership contract](../../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/Monitoring/Monitor.cpp`
