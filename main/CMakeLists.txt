# Main CMakeLists.txt for Plai-Meshcore
file(GLOB_RECURSE APPS_SRCS
    apps/*.c
    apps/*.cpp
)

file(GLOB_RECURSE HAL_SRCS
    hal/*.c
    hal/*.cpp
)

file(GLOB_RECURSE SETTINGS_SRCS
    settings/*.c
    settings/*.cpp
)

file(GLOB_RECURSE PROTOS_SRCS
    meshtastic/*.c
    meshtastic/*.cpp
)

file(GLOB_RECURSE MESH_SRCS
    mesh/*.c
    mesh/*.cpp
)

idf_component_register(SRCS "main.cpp" ${APPS_SRCS} ${HAL_SRCS} ${SETTINGS_SRCS} ${PROTOS_SRCS} ${MESH_SRCS}
                    INCLUDE_DIRS "." "hal" "meshtastic" "mesh/meshcore" "mesh/meshcore/ed25519"
                    REQUIRES LovyanGFX mooncake Nanopb fatfs esp_adc driver mbedtls
                    WHOLE_ARCHIVE
                    EMBED_FILES sound/error.wav sound/boot_sound.wav image/boot_logo.png sound/knock.wav sound/morse.wav sound/seagull.wav sound/pum_pam.wav sound/tum_tum.wav sound/eight_bit.wav sound/gps.wav sound/eagle.wav sound/parrot.wav sound/trace.wav sound/duck.wav sound/chicken.wav sound/woodpecker.wav sound/cb_press.wav sound/cb_release.wav
                    )
                    
                    
nvs_create_partition_image(nvs ../nvs.csv FLASH_IN_PROJECT)