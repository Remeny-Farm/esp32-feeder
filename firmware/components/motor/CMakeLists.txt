# Motor backend. Today: a logging stub. The real backend (Einsy RAMBo over UART)
# replaces motor_stub.c later; the API in include/motor.h stays.
idf_component_register(SRCS "motor_stub.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES log)
