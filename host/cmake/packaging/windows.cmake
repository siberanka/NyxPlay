# windows specific packaging
install(TARGETS sunshine RUNTIME DESTINATION "." COMPONENT application)

# Hardening: include zlib1.dll (loaded via LoadLibrary() in openssl's libcrypto.a)
install(FILES "${ZLIB}" DESTINATION "." COMPONENT application)

# ViGEmBus installer
set(VIGEMBUS_INSTALLER "${CMAKE_BINARY_DIR}/vigembus_installer.exe")
file(DOWNLOAD
        "https://github.com/nefarius/ViGEmBus/releases/download/v1.21.442.0/ViGEmBus_1.21.442_x64_x86_arm64.exe"
        ${VIGEMBUS_INSTALLER}
        SHOW_PROGRESS
        EXPECTED_HASH SHA256=155c50f1eec07bdc28d2f61a3e3c2c6c132fee7328412de224695f89143316bc
        TIMEOUT 60
)
install(FILES ${VIGEMBUS_INSTALLER}
        DESTINATION "scripts"
        RENAME "vigembus_installer.exe"
        COMPONENT gamepad)

# Adding tools
install(TARGETS dxgi-info RUNTIME DESTINATION "tools" COMPONENT dxgi)
install(TARGETS audio-info RUNTIME DESTINATION "tools" COMPONENT audio)

# Mandatory tools
install(TARGETS sunshinesvc RUNTIME DESTINATION "tools" COMPONENT application)

# Drivers
# The SudoVDA payload (driver DLL and the signed device installer nefconc.exe) is third-party,
# signed and git-ignored (*.dll, *.exe). It has to be placed next to the other driver files before
# packaging, otherwise the installer would not be able to install the virtual display driver.
# The files are pinned by SHA-256:
#   SudoVDA.dll   SudoVDA driver 1.10.9.289 (SudoMaker, as shipped with Apollo)
#   nefconc.exe   nefcon 1.12.0.0 (Nefarius Software Solutions e.U., BSD-3-Clause)
set(SUDOVDA_DRIVER_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda")
set(SUDOVDA_PAYLOAD
        "nefconc.exe=9dba1f1a9e2b21843c4a0ca6c6ffa9e747250dd6e42c8c14325ca069ad43ea8f"
        "SudoVDA.dll=47ee263cb5de9382c6630a2d7f3dafec4a49419f953beec869ca5dd0c460ff63")
foreach(_entry IN LISTS SUDOVDA_PAYLOAD)
    string(REGEX REPLACE "=.*$" "" _name "${_entry}")
    if(NOT EXISTS "${SUDOVDA_DRIVER_DIR}/${_name}")
        message(WARNING "SudoVDA driver file ${_name} is missing in ${SUDOVDA_DRIVER_DIR}: "
                "the installer package cannot be created until it is added")
    endif()
endforeach()
# Verified when the package is created, a missing or modified file stops the packaging step
install(CODE "
    foreach(_entry ${SUDOVDA_PAYLOAD})
        string(REGEX REPLACE \"=.*\$\" \"\" _name \"\${_entry}\")
        string(REGEX REPLACE \"^[^=]*=\" \"\" _expected \"\${_entry}\")
        set(_file \"${SUDOVDA_DRIVER_DIR}/\${_name}\")
        if(NOT EXISTS \"\${_file}\")
            message(FATAL_ERROR \"SudoVDA driver file \${_name} is missing in ${SUDOVDA_DRIVER_DIR}. \"
                    \"The installer cannot install the virtual display driver without it.\")
        endif()
        file(SHA256 \"\${_file}\" _actual)
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR \"SudoVDA driver file \${_name} does not match the expected release \"
                    \"(SHA-256 \${_actual}, expected \${_expected}).\")
        endif()
    endforeach()
    " COMPONENT sudovda)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda"
        DESTINATION "drivers"
        COMPONENT sudovda)

# Licence texts and third-party notices
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/licenses/"
        DESTINATION "licenses"
        COMPONENT application)
if(EXISTS "${PROJECT_SOURCE_DIR}/../CREDITS.md")
    install(FILES "${PROJECT_SOURCE_DIR}/../CREDITS.md"
            DESTINATION "licenses"
            COMPONENT application)
endif()

# Mandatory scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/migration/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/path/"
        DESTINATION "scripts"
        COMPONENT assets)

# Configurable options for the service
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/autostart/"
        DESTINATION "scripts"
        COMPONENT autostart)

# scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/firewall/"
        DESTINATION "scripts"
        COMPONENT firewall)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/gamepad/"
        DESTINATION "scripts"
        COMPONENT gamepad)

# Sunshine assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        COMPONENT assets)

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
# use junction for shaders directory
cmake_path(CONVERT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/shaders"
        TO_NATIVE_PATH_LIST shaders_in_build_src_native)
cmake_path(CONVERT "${CMAKE_BINARY_DIR}/assets/shaders" TO_NATIVE_PATH_LIST shaders_in_build_dest_native)
execute_process(COMMAND cmd.exe /c mklink /J "${shaders_in_build_dest_native}" "${shaders_in_build_src_native}")

set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}\\\\nyxplay-host.ico")

# The name of the directory that will be created in C:/Program files/
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_DISPLAY_NAME}")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${PROJECT_DISPLAY_NAME}")
set(CPACK_NSIS_PACKAGE_NAME "${PROJECT_DISPLAY_NAME}")
set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_DISPLAY_NAME}")
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${PROJECT_VERSION}-windows-x64")

# Setting components groups and dependencies
set(CPACK_COMPONENT_GROUP_CORE_EXPANDED true)

# sunshine binary
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "${PROJECT_DISPLAY_NAME}")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "${PROJECT_DISPLAY_NAME} main application and required components.")
set(CPACK_COMPONENT_APPLICATION_GROUP "Core")
set(CPACK_COMPONENT_APPLICATION_REQUIRED true)
set(CPACK_COMPONENT_APPLICATION_DEPENDS assets)

# service auto-start script
set(CPACK_COMPONENT_AUTOSTART_DISPLAY_NAME "Launch on Startup")
set(CPACK_COMPONENT_AUTOSTART_DESCRIPTION "If enabled, launches NyxPlay Host automatically on system startup.")
set(CPACK_COMPONENT_AUTOSTART_GROUP "Core")

# assets
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Required Assets")
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and web UI.")
set(CPACK_COMPONENT_ASSETS_GROUP "Core")
set(CPACK_COMPONENT_ASSETS_REQUIRED true)

# drivers
set(CPACK_COMPONENT_SUDOVDA_DISPLAY_NAME "SudoVDA")
set(CPACK_COMPONENT_SUDOVDA_DESCRIPTION "Driver required for Virtual Display to function. Shared with Apollo if it is installed. Adds its signing certificate to the machine Trusted Root and Trusted Publisher stores.")
set(CPACK_COMPONENT_SUDOVDA_GROUP "Drivers")
set(CPACK_COMPONENT_SUDOVDA_REQUIRED true)

# audio tool
set(CPACK_COMPONENT_AUDIO_DISPLAY_NAME "audio-info")
set(CPACK_COMPONENT_AUDIO_DESCRIPTION "CLI tool providing information about sound devices.")
set(CPACK_COMPONENT_AUDIO_GROUP "Tools")

# display tool
set(CPACK_COMPONENT_DXGI_DISPLAY_NAME "dxgi-info")
set(CPACK_COMPONENT_DXGI_DESCRIPTION "CLI tool providing information about graphics cards and displays.")
set(CPACK_COMPONENT_DXGI_GROUP "Tools")

# firewall scripts
set(CPACK_COMPONENT_FIREWALL_DISPLAY_NAME "Add Firewall Exclusions")
set(CPACK_COMPONENT_FIREWALL_DESCRIPTION "Scripts to enable or disable firewall rules.")
set(CPACK_COMPONENT_FIREWALL_GROUP "Scripts")

# gamepad scripts
set(CPACK_COMPONENT_GAMEPAD_DISPLAY_NAME "Virtual Gamepad")
set(CPACK_COMPONENT_GAMEPAD_DESCRIPTION "Scripts to install and uninstall Virtual Gamepad.")
set(CPACK_COMPONENT_GAMEPAD_GROUP "Scripts")

# include specific packaging
include(${CMAKE_MODULE_PATH}/packaging/windows_nsis.cmake)
include(${CMAKE_MODULE_PATH}/packaging/windows_wix.cmake)
