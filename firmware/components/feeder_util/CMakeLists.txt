# Pure C99 helpers (RFC 3339, HH:MM, random day planner). No ESP-IDF includes,
# so test/host builds the same sources with a plain C compiler.
idf_component_register(SRCS "rfc3339.c" "sched_plan.c"
                       INCLUDE_DIRS "include")
