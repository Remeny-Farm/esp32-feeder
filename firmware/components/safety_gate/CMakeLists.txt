# Pure C99 component: no ESP-IDF dependency (it is also built on the host by
# firmware/test/host/CMakeLists.txt).
idf_component_register(SRCS "safety_gate.c"
                       INCLUDE_DIRS "include")
