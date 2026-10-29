find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Runtime)
target_link_libraries(iiSocietyHelper PUBLIC iiSDKCore::Runtime)
option(IISOCIETYHELPER_WITH_BLE "Discover nearby peers and exchange bootstrap metadata over BLE" ON)
if(IISOCIETYHELPER_WITH_BLE)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Bluetooth)
    target_link_libraries(iiSocietyHelper PRIVATE Qt6::Bluetooth)
    target_compile_definitions(iiSocietyHelper PRIVATE IISOCIETYHELPER_BLE)
endif()
set(_helper_sync_dir src/Sync)
target_sources(iiSocietyHelper PRIVATE
    ${_helper_sync_dir}/Status.cpp ${_helper_sync_dir}/Status.h
    ${_helper_sync_dir}/Sync.cpp ${_helper_sync_dir}/Sync.h
    ${_helper_sync_dir}/Replica.cpp ${_helper_sync_dir}/Replica.h
    ${_helper_sync_dir}/ConfinedFiles.cpp ${_helper_sync_dir}/ConfinedFiles.h
    ${_helper_sync_dir}/NamespaceJournal.cpp ${_helper_sync_dir}/NamespaceJournal.h
    ${_helper_sync_dir}/Synchronizer.cpp ${_helper_sync_dir}/Synchronizer.h
    ${_helper_sync_dir}/Controller.cpp ${_helper_sync_dir}/Controller.h
    ${_helper_sync_dir}/RemoteFiles.cpp ${_helper_sync_dir}/RemoteFiles.h
    ${_helper_sync_dir}/NearbyBootstrap.cpp ${_helper_sync_dir}/NearbyBootstrap.h
    ${_helper_sync_dir}/BleDiscovery.cpp ${_helper_sync_dir}/BleDiscovery.h
    ${_helper_sync_dir}/ObjectProvider.cpp ${_helper_sync_dir}/ObjectProvider.h)
if(WIN32)
    target_sources(iiSocietyHelper PRIVATE ${_helper_sync_dir}/ConfinedFiles_win.cpp)
    target_link_libraries(iiSocietyHelper PRIVATE ntdll)
endif()
