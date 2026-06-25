idf_component_register(
  SRCS "user_app.cpp"
  PRIV_REQUIRES
  board_power_bsp
  driver
  main
  button_bsp
  i2c_equipment
  i2c_bsp
  adc_bsp
  nvs_flash
  fatfs
  wear_levelling
  esp_wifi
  esp_netif
  esp_event
  dms_crypto
  dms_vault
  dms_config
  dms_ui
  dms_web
  REQUIRES
  epaper_driver_bsp
  INCLUDE_DIRS ".")
