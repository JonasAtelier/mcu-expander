# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, chip folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file is empty
# on the other platform. Here the chip files are listed one by one, and
# PRIV_INCLUDE_DIRS "src" lets a chip folder reach expander_priv.h by plain
# name.
#
# The core is always built, even with CONFIG_EXPANDER off; each chip driver
# only when its Kconfig option is on. Kconfig values are available here
# during the build phase, and only SRCS depends on them -- never REQUIRES,
# which IDF resolves before sdkconfig. That is also why EXPANDER off still
# builds the core: an empty SRCS would need a second, differently-shaped
# register call, and an unreferenced core costs nothing once --gc-sections
# has run.
#
# REQUIRES driver, not esp_driver_i2c/spi: the split components do not exist
# on IDF 5.2, and driver still pulls them in on later versions.
set(srcs "src/expander.c" "src/expander_log.c" "src/expander_port_esp.c")

if(CONFIG_EXPANDER AND NOT CONFIG_EXPANDER_MCP23)
    message(WARNING "expander: CONFIG_EXPANDER is on but no chip driver is selected; "
                    "set CONFIG_EXPANDER_<CHIP>=y (esp-forge: EXPANDER_<CHIP> = y)")
endif()

if(CONFIG_EXPANDER_MCP23)
    list(APPEND srcs "src/mcp23/mcp23.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver)
