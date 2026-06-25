idf_component_register(
  SRCS "dms_web.c"
  INCLUDE_DIRS "include"
  REQUIRES dms_vault dms_config
  PRIV_REQUIRES esp_http_server)
