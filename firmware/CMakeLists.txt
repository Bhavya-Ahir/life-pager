cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(life_pager)

target_sources(app PRIVATE
  src/main.c src/hw.c src/ble.c src/modem.c
  src/model.c src/proto.c)

# 8x16 font for the 168x144 display (21 x 9 characters), generated from DejaVu Sans Mono
# the same way Zephyr's samples/subsys/display/cfb_custom_font does it.
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
generate_cfb_font_for_target(app fonts/DejaVuSansMono.ttf ${gen_dir}/cfb_font_pager.h 8 16
  --first 32 --last 126)
