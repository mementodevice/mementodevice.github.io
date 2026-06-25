idf_component_register(
  SRCS "dms_crypto.c"
  INCLUDE_DIRS "include"
  REQUIRES dms_secret
  PRIV_REQUIRES mbedtls esp_hw_support)
