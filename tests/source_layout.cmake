if(NOT SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
if(EXISTS "${SOURCE_ROOT}/src/iiSocietyHelper")
    message(FATAL_ERROR "The source tree must not repeat the project name")
endif()
foreach(_file Core/iiSocietyHelper.h Core/iiSocietyHelper.cpp Core/Helper.cpp
        Delivery/DeliveryStore.h Delivery/DeliveryStore.cpp Files/FileSystem.cpp
        Storage/Managed/ManagedStorage.cpp Storage/Canvas/CanvasStorage.cpp
        Storage/Generation/GenerationStorage.cpp Objects/ObjectCodec.cpp
        Account/AccountSession.h Account/AccountSession.cpp Client/Client.h Client/Client.cpp
        Discovery/NearbyDevices.h Discovery/NearbyDevices.cpp Storage/GroupSessionStore.h Storage/GroupSessionStore.cpp
        Media/FileDownload.h Media/FileDownload.cpp Media/MediaExport.h Media/MediaExport.cpp
        Sync/Replica.h Sync/Replica.cpp Monitoring/Monitor.h Monitoring/Monitor.cpp
        Generation/Remote.h Generation/Remote.cpp)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing source in its responsibility directory: ${_file}")
    endif()
endforeach()
