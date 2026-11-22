# The secrets header comes from SPIKE_SECRETS_DIR (kept in /dev/shm, never in the repo), else from this folder.
if(DEFINED ENV{SPIKE_SECRETS_DIR})
    set(spike_secrets_dir $ENV{SPIKE_SECRETS_DIR})
else()
    set(spike_secrets_dir ${CMAKE_CURRENT_LIST_DIR})
endif()
idf_component_register(SRCS "spike.c" INCLUDE_DIRS "." ${spike_secrets_dir}
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash mbedtls esp_timer)
