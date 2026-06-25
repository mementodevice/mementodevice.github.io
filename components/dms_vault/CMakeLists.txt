idf_component_register(
  SRCS "dms_vault.c"
  INCLUDE_DIRS "include"
  REQUIRES dms_crypto
  PRIV_REQUIRES json)
