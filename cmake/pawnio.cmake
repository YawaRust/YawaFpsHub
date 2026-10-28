# Бинарники не скачиваются при сборке. Любая замена требует повторного аудита и новых хешей.
function(yawa_embed_pawnio target)
    set(YAWA_PAWNIO_DIR "${CMAKE_SOURCE_DIR}/third_party/pawnio")
    set(files
        "PawnIO.sys|fca6e7d58b0cf38dbb913a2b9e532f48629145d395f454b16a9f58e97b8d3940"
        "PawnIO.inf|7c1c203e13693531243fbee3cb87d7b79170eae89f5729b3f41387fe68a54f0b"
        "pawnio.cat|a37d46840280efec92063d3a21014c803939e599b4c0da4a4d063b79eeca9446"
        "IntelMSR.bin|d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f"
        "AMDFamily17.bin|dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91"
        "SmbusPIIX4.bin|91f9b4b1c39e3d399ce48477a89d8f6bd3e58a2241064a4daec3a1513dff56e5"
        "sources.zip|46cb979754d6f2a6161c1aebc6b383f7ce1a1d611c8158cda4b61a9da795b47c")
    foreach(entry IN LISTS files)
        string(REPLACE "|" ";" pair "${entry}")
        list(GET pair 0 name)
        list(GET pair 1 expected)
        file(SHA256 "${YAWA_PAWNIO_DIR}/${name}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "PawnIO: SHA-256 mismatch for ${name}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${YAWA_PAWNIO_DIR}/${name}")
    endforeach()
    set(YAWA_PAWNIO_IDS "${CMAKE_SOURCE_DIR}/app/resources/pawnio_ids.h")
    configure_file("${CMAKE_SOURCE_DIR}/app/resources/pawnio.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/pawnio.rc" @ONLY)
    if(MSVC)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pawnio.rc")
    endif()
endfunction()
