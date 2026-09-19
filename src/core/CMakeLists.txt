# libopenartemis: engine library
set(OA_CORE_SRC
  version.cpp
  util/sha1.cpp
  util/charset.cpp
  util/path_utf8.cpp
  fs/project.cpp
  fs/compat_config.cpp
  fs/physfs_fs.cpp
  util/binary_stream.cpp
  runtime/runtime.cpp
  runtime/runtime_media.cpp
  runtime/runtime_save.cpp
  runtime/runtime_lua.cpp
  runtime/runtime_lua_file.cpp
  runtime/runtime_iet.cpp
  render/layer.cpp
  render/backend_gles.cpp
  render/font.cpp
  render/renderer.cpp
  render/text.cpp
  emote/psb_reader.cpp
  emote/emote_file.cpp
  emote/emote_render.cpp
  emote/emote_player.cpp
  emote/bc7decomp.cpp
  media/audio.cpp
  media/decode_pool.cpp
  media/image.cpp
  media/video.cpp
)
if(NOT OA_USE_SDL2)
  list(APPEND OA_CORE_SRC render/backend_sdl.cpp)
endif()

add_library(libopenartemis STATIC ${OA_CORE_SRC})
target_include_directories(libopenartemis PUBLIC
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(libopenartemis PUBLIC oa_deps)

if(MSVC)
  target_compile_options(libopenartemis PRIVATE /W4)
else()
  target_compile_options(libopenartemis PRIVATE -Wall -Wextra)
endif()
