# Auto-compile any generated LVGL font dropped into fonts/ (e.g. fonts/font_memento_14.c).
# (CONFIGURE_DEPENDS isn't allowed in IDF's component-scan script mode, so after ADDING a
# font file run `idf.py reconfigure` once -- or `idf.py fullclean` -- to pick it up.)
file(GLOB DMS_UI_FONTS "fonts/*.c")

idf_component_register(
  SRCS "dms_ui.c" "logo_img.c" ${DMS_UI_FONTS}
  INCLUDE_DIRS "include"
  REQUIRES dms_vault dms_config
  PRIV_REQUIRES lvgl button_bsp dms_secret)
