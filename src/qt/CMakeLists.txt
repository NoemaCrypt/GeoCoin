# GeoCoin Qt GUI

cmake_minimum_required(VERSION 3.10)

# Find Qt5
find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)

# Find qrencode for QR code generation
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(QRENCODE_PC QUIET libqrencode)
endif()

find_path(QRENCODE_INCLUDE_DIR NAMES qrencode.h HINTS ${QRENCODE_PC_INCLUDE_DIRS})
find_library(QRENCODE_LIBRARY_RELEASE NAMES qrencode libqrencode HINTS ${QRENCODE_PC_LIBRARY_DIRS})
find_library(QRENCODE_LIBRARY_DEBUG NAMES qrencoded libqrencoded HINTS ${QRENCODE_PC_LIBRARY_DIRS})

set(QRENCODE_FOUND FALSE)
if(QRENCODE_INCLUDE_DIR AND (QRENCODE_LIBRARY_RELEASE OR QRENCODE_LIBRARY_DEBUG))
    set(QRENCODE_FOUND TRUE)
    set(QRENCODE_INCLUDE_DIRS ${QRENCODE_INCLUDE_DIR})
    if(QRENCODE_LIBRARY_RELEASE AND QRENCODE_LIBRARY_DEBUG)
        set(QRENCODE_LIBRARIES
            optimized ${QRENCODE_LIBRARY_RELEASE}
            debug ${QRENCODE_LIBRARY_DEBUG}
        )
    elseif(QRENCODE_LIBRARY_RELEASE)
        set(QRENCODE_LIBRARIES ${QRENCODE_LIBRARY_RELEASE})
    else()
        set(QRENCODE_LIBRARIES ${QRENCODE_LIBRARY_DEBUG})
    endif()
endif()

mark_as_advanced(QRENCODE_INCLUDE_DIR QRENCODE_LIBRARY_RELEASE QRENCODE_LIBRARY_DEBUG)

# Enable automoc for Qt
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt sources
set(QT_SOURCES
    geocoin.cpp
    geocoincore.cpp
    geocoinapplication.cpp
    geocoingui.cpp
    forktreedialog.cpp
    clientmodel.cpp
    walletmodel.cpp
    walletsyncworker.cpp
    walletframe.cpp
    overviewpage.cpp
    sendcoinspage.cpp
    receivecoinspage.cpp
    transactionpage.cpp
    miningpage.cpp
    miningpage_gpu.cpp
    multisigpage.cpp
    multisigpage_initiator.cpp
    multisigpage_signer.cpp
    multisigpage_history.cpp
    multisigpage_scanner.cpp
    corporatepage.cpp
    corporatepage_founder.cpp
    corporatepage_director.cpp
    corporatepage_signer.cpp
    corporatepage_history.cpp
    nftpage.cpp
    nftpage_create.cpp
    commitmentscanner.cpp
    guiutil.cpp
    networkstyle.cpp
    optionsmodel.cpp
    transactiontablemodel.cpp
    addresstablemodel.cpp
    intro.cpp
)

# Qt headers
set(QT_HEADERS
    geocoincore.h
    geocoinapplication.h
    geocoingui.h
    forktreedialog.h
    clientmodel.h
    walletmodel.h
    walletsyncworker.h
    walletframe.h
    overviewpage.h
    sendcoinspage.h
    receivecoinspage.h
    transactionpage.h
    miningpage.h
    multisigpage.h
    corporatepage.h
    nftpage.h
    commitmentscanner.h
    guiutil.h
    networkstyle.h
    optionsmodel.h
    transactiontablemodel.h
    addresstablemodel.h
    intro.h
    managedasyncaction.h
)

# Qt resources
set(QT_RESOURCES
    geocoin.qrc
)

# Translations
set(QT_TRANSLATIONS
    translations/geocoin_en.ts
    translations/geocoin_ru.ts
)

# Create executable
add_executable(geocoin-qt
    ${QT_SOURCES}
    ${QT_HEADERS}
    ${QT_RESOURCES}
)

set_target_properties(geocoin-qt PROPERTIES
    OUTPUT_NAME geocoin-core
)

if(WIN32)
    target_sources(geocoin-qt PRIVATE geocoin-qt.rc)
    set_target_properties(geocoin-qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Build translations if Qt Linguist tools are available
find_package(Qt5LinguistTools QUIET)
if(Qt5LinguistTools_FOUND)
    qt5_add_translation(QM_FILES ${QT_TRANSLATIONS})
else()
    find_program(LRELEASE_EXECUTABLE lrelease)
    if(LRELEASE_EXECUTABLE)
        foreach(_ts IN LISTS QT_TRANSLATIONS)
            get_filename_component(_ts_name ${_ts} NAME_WE)
            set(_qm "${CMAKE_CURRENT_BINARY_DIR}/${_ts_name}.qm")
            add_custom_command(
                OUTPUT ${_qm}
                COMMAND ${LRELEASE_EXECUTABLE} -qm ${_qm} ${CMAKE_CURRENT_SOURCE_DIR}/${_ts}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_ts}
                COMMENT "Generating ${_qm} from ${_ts}"
                VERBATIM
            )
            list(APPEND QM_FILES ${_qm})
        endforeach()
    endif()
endif()

if(QM_FILES)
    add_custom_target(geocoin_translations ALL DEPENDS ${QM_FILES})
    add_dependencies(geocoin-qt geocoin_translations)
endif()

# Link libraries
target_link_libraries(geocoin-qt
    PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    geocoin_core
    geocoin_consensus
    geocoin_crypto
    geocoin_network
    geocoin_rpc
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_node
    Threads::Threads
)

if(WIN32 AND NOT GEOCOIN_WINDOWS_STATIC_RELEASE)
    foreach(_qt_dynamic_platform_plugin
            Qt5::QWindowsIntegrationPlugin
            Qt5::QOffscreenIntegrationPlugin)
        if(TARGET ${_qt_dynamic_platform_plugin})
            add_custom_command(
                TARGET geocoin-qt
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:geocoin-qt>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${_qt_dynamic_platform_plugin}>"
                        "$<TARGET_FILE_DIR:geocoin-qt>/platforms"
            )
        endif()
    endforeach()
endif()

if(WIN32 AND GEOCOIN_WINDOWS_STATIC_RELEASE)
    target_compile_definitions(geocoin-qt PRIVATE GEOCOIN_QT_STATIC_PLUGINS QT_STATICPLUGIN)
    if(COMMAND qt5_import_plugins)
        qt5_import_plugins(geocoin-qt
            INCLUDE
                Qt5::QWindowsIntegrationPlugin
                Qt5::QOffscreenIntegrationPlugin
                Qt5::QICOPlugin
        )
    endif()
    foreach(_qt_static_plugin
            Qt5::QWindowsIntegrationPlugin
            Qt5::QOffscreenIntegrationPlugin
            Qt5::QICOPlugin)
        if(TARGET ${_qt_static_plugin})
            target_link_libraries(geocoin-qt PRIVATE ${_qt_static_plugin})
        endif()
    endforeach()
    if(MSVC)
        set(_geocoin_msvc_static_runtime "")
        set(_geocoin_msvc_runtime_candidates)
        foreach(_geocoin_msvc_root
                "$ENV{VCToolsInstallDir}"
                "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/MSVC"
                "$ENV{VSINSTALLDIR}/VC/Tools/MSVC"
                "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"
                "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC"
                "D:/VS/VC/Tools/MSVC"
                "D:/Visual Studio/Community/VC/Tools/MSVC")
            if(_geocoin_msvc_root)
                file(TO_CMAKE_PATH "${_geocoin_msvc_root}" _geocoin_msvc_root)
                if(EXISTS "${_geocoin_msvc_root}/lib/x64/libcpmt.lib")
                    list(APPEND _geocoin_msvc_runtime_candidates "${_geocoin_msvc_root}/lib/x64/libcpmt.lib")
                else()
                    file(GLOB _geocoin_msvc_globbed_libcpmt
                        "${_geocoin_msvc_root}/*/lib/x64/libcpmt.lib")
                    list(APPEND _geocoin_msvc_runtime_candidates ${_geocoin_msvc_globbed_libcpmt})
                endif()
            endif()
        endforeach()

        if(_geocoin_msvc_runtime_candidates)
            list(REMOVE_DUPLICATES _geocoin_msvc_runtime_candidates)
            set(_geocoin_msvc_best_version "0")
            foreach(_geocoin_msvc_candidate ${_geocoin_msvc_runtime_candidates})
                get_filename_component(_geocoin_msvc_arch_dir "${_geocoin_msvc_candidate}" DIRECTORY)
                get_filename_component(_geocoin_msvc_lib_dir "${_geocoin_msvc_arch_dir}" DIRECTORY)
                get_filename_component(_geocoin_msvc_version_dir "${_geocoin_msvc_lib_dir}" DIRECTORY)
                get_filename_component(_geocoin_msvc_version "${_geocoin_msvc_version_dir}" NAME)
                if(_geocoin_msvc_version VERSION_GREATER _geocoin_msvc_best_version)
                    set(_geocoin_msvc_best_version "${_geocoin_msvc_version}")
                    set(_geocoin_msvc_static_runtime "${_geocoin_msvc_candidate}")
                endif()
            endforeach()
        endif()

        if(_geocoin_msvc_static_runtime)
            message(STATUS "GeoCoin Qt static runtime: ${_geocoin_msvc_static_runtime}")
            target_link_libraries(geocoin-qt PRIVATE "${_geocoin_msvc_static_runtime}")
        else()
            message(WARNING "GeoCoin Qt static runtime libcpmt.lib was not found; Clang static Qt link may fail on new MSVC STL symbols.")
        endif()
    endif()
endif()

# Link qrencode if found
if(QRENCODE_FOUND)
    target_link_libraries(geocoin-qt PRIVATE ${QRENCODE_LIBRARIES})
    target_include_directories(geocoin-qt PRIVATE ${QRENCODE_INCLUDE_DIRS})
    target_compile_definitions(geocoin-qt PRIVATE USE_QRCODE)
endif()

# Include directories
target_include_directories(geocoin-qt PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Install
install(TARGETS geocoin-qt DESTINATION bin)
if(COMMAND geocoin_stage_runtime_dependencies)
    geocoin_stage_runtime_dependencies(geocoin-qt)
endif()

if(WIN32 AND NOT GEOCOIN_WINDOWS_STATIC_RELEASE)
    get_target_property(QT_QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
    if(QT_QMAKE_EXECUTABLE)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        set(QT_PLATFORMS_DIR "${QT_BIN_DIR}/../plugins/platforms")
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET geocoin-qt POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-compiler-runtime --no-translations "$<TARGET_FILE:geocoin-qt>"
                COMMENT "Bundling Qt runtime for geocoin-qt"
                VERBATIM
            )
        endif()
        if(EXISTS "${QT_PLATFORMS_DIR}/qoffscreen.dll")
            add_custom_command(TARGET geocoin-qt POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:geocoin-qt>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QT_PLATFORMS_DIR}/qoffscreen.dll"
                        "$<TARGET_FILE_DIR:geocoin-qt>/platforms/qoffscreen.dll"
                COMMENT "Copying Qt offscreen platform plugin for geocoin-qt"
                VERBATIM
            )
        endif()
        if(EXISTS "${QT_PLATFORMS_DIR}/qoffscreend.dll")
            add_custom_command(TARGET geocoin-qt POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:geocoin-qt>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QT_PLATFORMS_DIR}/qoffscreend.dll"
                        "$<TARGET_FILE_DIR:geocoin-qt>/platforms/qoffscreend.dll"
                COMMENT "Copying Qt debug offscreen platform plugin for geocoin-qt"
                VERBATIM
            )
        endif()
    endif()
endif()
