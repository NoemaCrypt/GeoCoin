# The native AppKit target retains the Qt GUI's target and executable names.

# .mm bridge sources need the Objective-C++ language enabled (Darwin-only dir).
enable_language(OBJC OBJCXX)

# Find Qt5 (Core/Gui power the reused models; Widgets only for Qt types the
# models touch, e.g. QDialog-based panels land later with Shell/Pages).
find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
find_package(Qt5LinguistTools ${Qt5Core_VERSION_STRING} EXACT CONFIG REQUIRED)

# Find qrencode for QR code generation (same block as src/qt/CMakeLists.txt:8-34)
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

# Enable automoc for the reused Qt model and workflow classes.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)

# Share GUI startup, models and workflow pages with the Qt frontend.
set(GEOCOIN_MACOS_REUSED_MODELS
    ${PROJECT_SOURCE_DIR}/src/qt/geocoincore.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/geocoin.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/geocoinapplication.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/geocoingui.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/geocoingui_layouttest.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/forktreedialog.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/clientmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/walletmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/walletsyncworker.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/walletframe.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/overviewpage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/sendcoinspage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/receivecoinspage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/transactionpage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/miningpage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/miningpage_gpu.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage_initiator.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage_signer.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage_history.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage_scanner.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage_founder.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage_director.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage_signer.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage_history.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/nftpage.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/nftpage_create.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/commitmentscanner.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/guiutil.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/networkstyle.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/optionsmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/transactiontablemodel.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/addresstablemodel.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/intro.cpp
    ${PROJECT_SOURCE_DIR}/src/qt/geocoincore.h
    ${PROJECT_SOURCE_DIR}/src/qt/geocoinapplication.h
    ${PROJECT_SOURCE_DIR}/src/qt/geocoingui.h
    ${PROJECT_SOURCE_DIR}/src/qt/forktreedialog.h
    ${PROJECT_SOURCE_DIR}/src/qt/clientmodel.h
    ${PROJECT_SOURCE_DIR}/src/qt/walletmodel.h
    ${PROJECT_SOURCE_DIR}/src/qt/walletsyncworker.h
    ${PROJECT_SOURCE_DIR}/src/qt/walletframe.h
    ${PROJECT_SOURCE_DIR}/src/qt/overviewpage.h
    ${PROJECT_SOURCE_DIR}/src/qt/sendcoinspage.h
    ${PROJECT_SOURCE_DIR}/src/qt/receivecoinspage.h
    ${PROJECT_SOURCE_DIR}/src/qt/transactionpage.h
    ${PROJECT_SOURCE_DIR}/src/qt/miningpage.h
    ${PROJECT_SOURCE_DIR}/src/qt/multisigpage.h
    ${PROJECT_SOURCE_DIR}/src/qt/corporatepage.h
    ${PROJECT_SOURCE_DIR}/src/qt/nftpage.h
    ${PROJECT_SOURCE_DIR}/src/qt/commitmentscanner.h
    ${PROJECT_SOURCE_DIR}/src/qt/guiutil.h
    ${PROJECT_SOURCE_DIR}/src/qt/networkstyle.h
    ${PROJECT_SOURCE_DIR}/src/qt/optionsmodel.h
    ${PROJECT_SOURCE_DIR}/src/qt/transactiontablemodel.h
    ${PROJECT_SOURCE_DIR}/src/qt/addresstablemodel.h
    ${PROJECT_SOURCE_DIR}/src/qt/intro.h
    ${PROJECT_SOURCE_DIR}/src/qt/managedasyncaction.h
    ${PROJECT_SOURCE_DIR}/src/qt/qtlogging.h
)

# Native AppKit shell and bridge.
set(GEOCOIN_MACOS_SOURCES
    main.mm
    Bridge/GeoCoinCoreBridge.mm
    Bridge/GeoGpuMinerLauncher.m
    Theme/GeoCoinTheme.m
    Theme/GeoCoinPageKit.m
    Theme/GeoCoinMetalBackdrop.mm
    Shell/GeoCoinMainWindowController.m
    Shell/GeoCoinIntroSheet.m
    Pages/GeoCoinOverviewPage.m
    Pages/GeoCoinSendCoinsPage.m
    Pages/GeoCoinReceiveCoinsPage.m
    Pages/GeoCoinTransactionPage.m
    Pages/GeoCoinMiningPage.m
    Pages/GeoCoinMultisigPage.m
    Pages/GeoCoinCorporatePage.m
    Pages/GeoCoinNftPage.m
)

set(GEOCOIN_MACOS_HEADERS
    GeoCoinNativeFrontend.h
    Bridge/GeoCoinCoreBridge.h
    Bridge/GeoGpuMinerLauncher.h
    Theme/GeoCoinTheme.h
    Theme/GeoCoinPageKit.h
    Theme/GeoCoinMetalBackdrop.h
    Shell/GeoCoinMainWindowController.h
    Shell/GeoCoinIntroSheet.h
    Pages/GeoCoinOverviewPage.h
    Pages/GeoCoinSendCoinsPage.h
    Pages/GeoCoinReceiveCoinsPage.h
    Pages/GeoCoinTransactionPage.h
    Pages/GeoCoinMiningPage.h
    Pages/GeoCoinMultisigPage.h
    Pages/GeoCoinCorporatePage.h
    Pages/GeoCoinNftPage.h
)

# Keep the app's translations in its own build directory. Compile source
# translations with the same Qt version as the app, including Mac-only updates.
set(QT_DIR "${PROJECT_SOURCE_DIR}/src/qt")
set(QT_EN "${QT_DIR}/translations/geocoin_en.ts")
set(QT_RU "${QT_DIR}/translations/geocoin_ru.ts")
set(MACOS_RU "${CMAKE_CURRENT_SOURCE_DIR}/translations/geocoin_ru_overlay.ts")
set(MERGED_RU "${CMAKE_CURRENT_BINARY_DIR}/geocoin_ru_macos.ts")
set(QM_EN "${CMAKE_CURRENT_BINARY_DIR}/geocoin_en.qm")
set(QM_RU "${CMAKE_CURRENT_BINARY_DIR}/geocoin_ru.qm")
add_custom_command(
    OUTPUT "${QM_EN}"
    COMMAND Qt5::lrelease "${QT_EN}" -qm "${QM_EN}"
    DEPENDS "${QT_EN}" Qt5::lrelease
    VERBATIM
)
add_custom_command(
    OUTPUT "${QM_RU}"
    BYPRODUCTS "${MERGED_RU}"
    COMMAND Qt5::lconvert "${QT_RU}" "${MACOS_RU}" -o "${MERGED_RU}"
    COMMAND Qt5::lrelease "${MERGED_RU}" -qm "${QM_RU}"
    DEPENDS "${QT_RU}" "${MACOS_RU}" Qt5::lrelease Qt5::lconvert
    VERBATIM
)

set(QT_ART "${QT_DIR}/res")
configure_file(geocoin.qrc.in "${CMAKE_CURRENT_BINARY_DIR}/geocoin_macos.qrc" @ONLY)
qt5_add_resources(QT_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/geocoin_macos.qrc")

# Create executable (same target name as src/qt variant; flat binary, not .app)
add_executable(geocoin-qt
    ${GEOCOIN_MACOS_SOURCES}
    ${GEOCOIN_MACOS_HEADERS}
    ${GEOCOIN_MACOS_REUSED_MODELS}
    ${QT_RESOURCES}
)

set_target_properties(geocoin-qt PROPERTIES
    OUTPUT_NAME geocoin-core
    MACOSX_BUNDLE FALSE
)

# ARC: the bridge uses __weak references (MRC does not support them).
target_compile_options(geocoin-qt PRIVATE "$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>")
target_compile_definitions(geocoin-qt PRIVATE GEOCOIN_NATIVE_MACOS=1)
target_include_directories(geocoin-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Bridge mixes Objective-C with C++ Qt headers -> Objective-C++.
set_source_files_properties(Bridge/GeoCoinCoreBridge.mm PROPERTIES LANGUAGE OBJCXX)

# Link libraries (geocoin_* set mirrors src/qt/CMakeLists.txt:163-178)
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
    "-framework AppKit"
    "-framework CoreImage"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
)

# Link qrencode if found
if(QRENCODE_FOUND)
    target_link_libraries(geocoin-qt PRIVATE ${QRENCODE_LIBRARIES})
    target_include_directories(geocoin-qt PRIVATE ${QRENCODE_INCLUDE_DIRS})
    target_compile_definitions(geocoin-qt PRIVATE USE_QRCODE)
endif()

# Include directories. Native dirs come FIRST: on case-insensitive macOS FS
# "GeoCoinCoreBridge.h" would otherwise resolve to src/qt/geocoincore.h (C++ class).
target_include_directories(geocoin-qt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/Theme
    ${CMAKE_CURRENT_SOURCE_DIR}/Shell
    ${CMAKE_CURRENT_SOURCE_DIR}/Pages
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/qt
)

# Install
install(TARGETS geocoin-qt DESTINATION bin)
if(COMMAND geocoin_stage_runtime_dependencies)
    geocoin_stage_runtime_dependencies(geocoin-qt)
endif()
