cmake_minimum_required(VERSION 3.6)

set(SOURCE_FILES
        antpatt.c
        antpatt.h
        audio_bridge.c
        audio_bridge.h
        audio_bridge_debug.c
        audio_bridge_debug.h
        audio_stream.c
        audio_stream.h
        conf.c
        conf.h
        conf-defaults.h
        fonts.c
        fonts.h
        log.c
        log.h
        main.c
        rds-utils.c
        rds-utils.h
        rdsspy.c
        rdsspy.h
        scan.c
        scan.h
        scheduler.c
        scheduler.h
        settings.c
        settings.h
        stationlist.c
        stationlist.h
        tuner.c
        tuner.h
        tuner-callbacks.c
        tuner-callbacks.h
        tuner-conn.c
        tuner-conn.h
        tuner-filters.c
        tuner-filters.h
        tuner-scan.c
        tuner-scan.h
        ui.c
        ui.h
        ui-audio.c
        ui-audio.h
        ui-connect.c
        ui-connect.h
        ui-input.c
        ui-input.h
        ui-signal.c
        ui-signal.h
        ui-tuner-set.c
        ui-tuner-set.h
        ui-tuner-update.c
        ui-tuner-update.h
        version.h
        ${CMAKE_BINARY_DIR}/resources.c)

set(SOURCE_FILES_MINGW
        win32.c
        win32.h
        icon.rc)

set(LIBRARIES
        rdsparser_static
        ${GTK_LIBRARIES}
        opus
        m)

set(LIBRARIES_MINGW
        ws2_32
        gdi32
        ole32
        oleaut32
        uuid
        avrt)

if(MINGW)
    IF(NOT (CMAKE_BUILD_TYPE MATCHES Debug))
        # -mwindows           : GUI subsystem (no console)
        # -static-libgcc      : avoid external libgcc_s_seh-1.dll dependency
        # -Wl,-s              : strip symbols at link time (smaller PE, fewer
        #                       artefacts that Defender's heuristics flag)
        # -Wl,--no-insert-timestamp : reproducible build, slightly more
        #                             reputation-friendly
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mwindows -static-libgcc")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -Wl,-s -Wl,--no-insert-timestamp")
    ENDIF()
    set(CMAKE_RC_COMPILER_INIT windres)
    ENABLE_LANGUAGE(RC)
    SET(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -i <SOURCE> -o <OBJECT>")
    add_executable(xdr-gtk ${SOURCE_FILES} ${SOURCE_FILES_MINGW})
    target_link_libraries(xdr-gtk ${LIBRARIES} ${LIBRARIES_MINGW})
ELSE()
    add_executable(xdr-gtk ${SOURCE_FILES})
    target_link_libraries(xdr-gtk ${LIBRARIES})
ENDIF()

target_include_directories(xdr-gtk PRIVATE ${CMAKE_BINARY_DIR})
