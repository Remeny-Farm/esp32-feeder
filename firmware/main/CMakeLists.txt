# The three certificate files are gitignored; see ../certs/README.md. The build
# fails if one is missing. EMBED_TXTFILES appends a NUL byte, which the TLS
# stack needs to read the PEM text.
idf_component_register(
    SRCS "app_main.c"
         "feeder.c"
         "feeder_mqtt.c"
         "fclock.c"
         "gate_config.c"
         "nv_store.c"
         "proto.c"
         "rand_sched.c"
         "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES safety_gate feeder_util motor cue
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer lwip mqtt json
                  app_update esp_app_format driver
    EMBED_TXTFILES "../certs/ca.crt" "../certs/device.crt" "../certs/device.key")
