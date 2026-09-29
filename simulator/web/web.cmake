# The web demo's half of the simulator build, included by
# simulator/CMakeLists.txt when it is compiled with Emscripten (see
# simulator/CLAUDE.md, "Web build seam"). PICODECK_WEB_DIR names the directory
# holding this file: simulator/web by default, or a PicoDeck/web-sim checkout.
# It sets the web platform and network-stub sources and defines
# picodeck_web_package(target), which builds the demo's SD image and links the
# page once the simulator target exists.
set(PICODECK_WEB_PLATFORM_SOURCES ${PICODECK_WEB_DIR}/web_platform.c)
set(PICODECK_WEB_NET_SOURCES ${PICODECK_WEB_DIR}/web_net_stubs.c)

# Lua apps bundled into the demo's SD image: names of dirs under apps/, or
# absolute paths to app dirs that live in their own repos.
set(PICODECK_WEB_APPS "snake;nonogram;calculator;editor;3dtest"
    CACHE STRING "Apps to bundle into the web demo SD image")
# Store apps bundled from their release ZIPs, pinned by hash:
# "<dir under /apps>|<zip URL>|<SHA-256>". Downloaded at configure time
# into web_downloads/ and reused while the hash still matches.
set(PICODECK_WEB_STORE_APPS
    "blockexe|https://github.com/PicoDeck/blockexe/releases/download/v1.0.3/picodeck-blockexe-v1.0.3.zip|66cc0c9ebc251e704a78ecf540b94ca6bab2055bf66f96ea550319e9a8dbcac7"
    CACHE STRING "Store app release ZIPs to bundle into the web demo SD image")

function(picodeck_web_package target)
    set(WEB_SD_DIR ${CMAKE_BINARY_DIR}/web_sd)
    file(REMOVE_RECURSE ${WEB_SD_DIR})
    file(MAKE_DIRECTORY ${WEB_SD_DIR}/apps ${WEB_SD_DIR}/data)
    file(COPY ${PICODECK_ROOT}/system DESTINATION ${WEB_SD_DIR})
    foreach(app ${PICODECK_WEB_APPS})
        if(IS_ABSOLUTE ${app})
            set(app_src ${app})
        else()
            set(app_src ${PICODECK_ROOT}/apps/${app})
        endif()
        if(NOT IS_DIRECTORY ${app_src})
            message(FATAL_ERROR "PICODECK_WEB_APPS: ${app_src} is not a directory")
        endif()
        file(COPY ${app_src} DESTINATION ${WEB_SD_DIR}/apps
             PATTERN ".git" EXCLUDE PATTERN ".github" EXCLUDE
             PATTERN "tests" EXCLUDE PATTERN "tools" EXCLUDE
             PATTERN "*.md" EXCLUDE)
    endforeach()
    foreach(entry ${PICODECK_WEB_STORE_APPS})
        string(REPLACE "|" ";" fields "${entry}")
        list(LENGTH fields nfields)
        if(NOT nfields EQUAL 3)
            message(FATAL_ERROR "PICODECK_WEB_STORE_APPS: '${entry}' is not <dir>|<url>|<sha256>")
        endif()
        list(GET fields 0 app_dir)
        list(GET fields 1 app_url)
        list(GET fields 2 app_sha)
        set(app_zip ${CMAKE_BINARY_DIR}/web_downloads/${app_dir}.zip)
        set(have_sha "")
        if(EXISTS ${app_zip})
            file(SHA256 ${app_zip} have_sha)
        endif()
        if(NOT have_sha STREQUAL app_sha)
            file(DOWNLOAD ${app_url} ${app_zip} TLS_VERIFY ON
                 EXPECTED_HASH SHA256=${app_sha} STATUS dl_status)
            list(GET dl_status 0 dl_code)
            if(NOT dl_code EQUAL 0)
                message(FATAL_ERROR "PICODECK_WEB_STORE_APPS: ${app_url}: ${dl_status}")
            endif()
        endif()
        file(ARCHIVE_EXTRACT INPUT ${app_zip} DESTINATION ${WEB_SD_DIR}/apps/${app_dir})
        if(NOT EXISTS ${WEB_SD_DIR}/apps/${app_dir}/main.lua)
            message(FATAL_ERROR "PICODECK_WEB_STORE_APPS: ${app_url} has no main.lua at its root")
        endif()
    endforeach()

    set_target_properties(${target} PROPERTIES
        SUFFIX ".html"
        LINK_DEPENDS ${PICODECK_WEB_DIR}/shell.html)
    # The page glue shell.html loads (and picodeck.net/try/ loads from the zip).
    configure_file(${PICODECK_WEB_DIR}/shell.js ${CMAKE_BINARY_DIR}/shell.js COPYONLY)
    target_link_options(${target} PRIVATE
        -O2
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=262144
        -sSTACK_SIZE=1048576
        -sALLOW_MEMORY_GROWTH
        -sINITIAL_MEMORY=64MB
        -sEXIT_RUNTIME=0
        -lidbfs.js
        -sEXPORTED_RUNTIME_METHODS=FS
        --preload-file ${WEB_SD_DIR}@/sd
        --shell-file ${PICODECK_WEB_DIR}/shell.html
    )
endfunction()
