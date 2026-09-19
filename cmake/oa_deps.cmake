# Shared dependency wiring for Windows (MSYS2 pkg-config) and HarmonyOS prebuilts.

if(NOT TARGET oa_deps)
  add_library(oa_deps INTERFACE)
endif()

set(OA_SDL2_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/src/platform/sdl2_bridge")

# ----- Vendored Lua -----
set(LUA51_SRC_DIR ${CMAKE_SOURCE_DIR}/third_party/lua-5.1.5/src)
if(NOT TARGET lua51)
  add_library(lua51 STATIC
    ${LUA51_SRC_DIR}/lapi.c ${LUA51_SRC_DIR}/lauxlib.c ${LUA51_SRC_DIR}/lbaselib.c
    ${LUA51_SRC_DIR}/lcode.c ${LUA51_SRC_DIR}/ldblib.c ${LUA51_SRC_DIR}/ldebug.c
    ${LUA51_SRC_DIR}/ldo.c ${LUA51_SRC_DIR}/ldump.c ${LUA51_SRC_DIR}/lfunc.c
    ${LUA51_SRC_DIR}/lgc.c ${LUA51_SRC_DIR}/linit.c ${LUA51_SRC_DIR}/liolib.c
    ${LUA51_SRC_DIR}/llex.c ${LUA51_SRC_DIR}/lmathlib.c ${LUA51_SRC_DIR}/lmem.c
    ${LUA51_SRC_DIR}/loadlib.c ${LUA51_SRC_DIR}/lobject.c ${LUA51_SRC_DIR}/lopcodes.c
    ${LUA51_SRC_DIR}/loslib.c ${LUA51_SRC_DIR}/lparser.c ${LUA51_SRC_DIR}/lstate.c
    ${LUA51_SRC_DIR}/lstring.c ${LUA51_SRC_DIR}/lstrlib.c ${LUA51_SRC_DIR}/ltable.c
    ${LUA51_SRC_DIR}/ltablib.c ${LUA51_SRC_DIR}/ltm.c ${LUA51_SRC_DIR}/lundump.c
    ${LUA51_SRC_DIR}/lvm.c ${LUA51_SRC_DIR}/lzio.c ${LUA51_SRC_DIR}/print.c
  )
  target_include_directories(lua51 PUBLIC ${LUA51_SRC_DIR})
  target_compile_options(lua51 PRIVATE -w)
  if(NOT MSVC)
    target_compile_options(lua51 PRIVATE -fPIC)
  endif()
endif()

# ----- Vendored PhysicsFS -----
set(OA_PHYSFS_DIR ${CMAKE_SOURCE_DIR}/third_party/physfs)
if(NOT TARGET physfs_vendored)
  set(_OA_PHYSFS_SRC
    ${OA_PHYSFS_DIR}/physfs.c
    ${OA_PHYSFS_DIR}/physfs_byteorder.c
    ${OA_PHYSFS_DIR}/physfs_unicode.c
    ${OA_PHYSFS_DIR}/physfs_archiver_dir.c
    ${OA_PHYSFS_DIR}/physfs_archiver_unpacked.c
    ${OA_PHYSFS_DIR}/physfs_archiver_zip.c
    ${OA_PHYSFS_DIR}/physfs_archiver_7z.c
    ${OA_PHYSFS_DIR}/physfs_archiver_grp.c
    ${OA_PHYSFS_DIR}/physfs_archiver_hog.c
    ${OA_PHYSFS_DIR}/physfs_archiver_iso9660.c
    ${OA_PHYSFS_DIR}/physfs_archiver_mvl.c
    ${OA_PHYSFS_DIR}/physfs_archiver_qpak.c
    ${OA_PHYSFS_DIR}/physfs_archiver_slb.c
    ${OA_PHYSFS_DIR}/physfs_archiver_vdf.c
    ${OA_PHYSFS_DIR}/physfs_archiver_wad.c
  )
  if(WIN32 AND NOT OHOS)
    list(APPEND _OA_PHYSFS_SRC ${OA_PHYSFS_DIR}/physfs_platform_windows.c)
  else()
    list(APPEND _OA_PHYSFS_SRC
      ${OA_PHYSFS_DIR}/physfs_platform_posix.c
      ${OA_PHYSFS_DIR}/physfs_platform_unix.c)
  endif()
  add_library(physfs_vendored STATIC ${_OA_PHYSFS_SRC})
  target_include_directories(physfs_vendored PUBLIC ${OA_PHYSFS_DIR})
  target_compile_definitions(physfs_vendored PRIVATE PHYSFS_STATIC)
  if(OHOS)
    target_compile_definitions(physfs_vendored PRIVATE PHYSFS_NO_CDROM_SUPPORT=1)
  endif()
  target_compile_options(physfs_vendored PRIVATE -w)
endif()

function(oa_link_prebuilt target libname)
  find_library(_OA_LIB_${libname} NAMES ${libname} ${ARGN} PATHS ${PREBUILT_LIBS_DIR} NO_DEFAULT_PATH)
  if(NOT _OA_LIB_${libname})
    message(WARNING "Prebuilt ${libname} not found in ${PREBUILT_LIBS_DIR}; linking by name")
  endif()
  # Link by short name so DT_NEEDED stays "libz.so" instead of a Windows
  # absolute path (libraries without an embedded SONAME would otherwise
  # record the find_library result verbatim).
  target_link_libraries(${target} INTERFACE ${libname})
endfunction()

if(OHOS)
  if(NOT DEFINED PREBUILT_LIBS_DIR)
    message(FATAL_ERROR "PREBUILT_LIBS_DIR must be set for HarmonyOS build")
  endif()
  if(NOT DEFINED SDL2_INCLUDE_DIR)
    message(FATAL_ERROR "SDL2_INCLUDE_DIR must be set for HarmonyOS build")
  endif()
  if(NOT DEFINED FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFMPEG_INCLUDE_DIR must be set for HarmonyOS build")
  endif()

  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
  target_include_directories(oa_deps INTERFACE
    ${OA_SDL2_BRIDGE_DIR}
    ${SDL2_INCLUDE_DIR}
  )
  target_link_directories(oa_deps INTERFACE ${PREBUILT_LIBS_DIR})
  oa_link_prebuilt(oa_deps SDL2)
  if(HARMONY_DEPS_INCLUDE_DIR)
    target_include_directories(oa_deps INTERFACE
      ${HARMONY_DEPS_INCLUDE_DIR}
      ${HARMONY_DEPS_INCLUDE_DIR}/freetype2)
  endif()
  oa_link_prebuilt(oa_deps freetype)
  oa_link_prebuilt(oa_deps png16 png)
  oa_link_prebuilt(oa_deps jpeg)
  oa_link_prebuilt(oa_deps turbojpeg)
  oa_link_prebuilt(oa_deps ogg)
  oa_link_prebuilt(oa_deps z)
  target_include_directories(oa_deps INTERFACE ${FFMPEG_INCLUDE_DIR})
  foreach(_fflib avcodec avformat avutil swscale swresample)
    oa_link_prebuilt(oa_deps ${_fflib})
  endforeach()
  target_compile_definitions(oa_deps INTERFACE OA_HAVE_FFMPEG=1)

  # Vorbis from SDL_mixer externals (same as KR2)
  set(_VORBIS_SRC ${CMAKE_SOURCE_DIR}/../../SDL2/SDL_mixer/external/vorbis)
  set(_OGG_INC ${CMAKE_SOURCE_DIR}/../../SDL2/SDL_mixer/external/ogg/include)
  if(NOT TARGET vorbis_vendored AND EXISTS "${_VORBIS_SRC}/lib")
    file(GLOB _VORBIS_SOURCES ${_VORBIS_SRC}/lib/*.c)
    list(FILTER _VORBIS_SOURCES EXCLUDE REGEX "(barkmel|psytune|tone)\\.c$")
    add_library(vorbis_vendored STATIC ${_VORBIS_SOURCES})
    target_include_directories(vorbis_vendored PRIVATE
      ${_VORBIS_SRC}/include ${_VORBIS_SRC}/lib ${_OGG_INC})
    target_compile_options(vorbis_vendored PRIVATE -w)
    target_link_libraries(oa_deps INTERFACE vorbis_vendored)
    target_include_directories(oa_deps INTERFACE ${_VORBIS_SRC}/include ${_OGG_INC})
  endif()

  # Decode-only Theora
  set(_THEORA_DIR ${CMAKE_SOURCE_DIR}/third_party/libtheora)
  if(NOT TARGET theora_vendored)
    add_library(theora_vendored STATIC
      ${_THEORA_DIR}/apiwrapper.c
      ${_THEORA_DIR}/bitpack.c
      ${_THEORA_DIR}/decapiwrapper.c
      ${_THEORA_DIR}/decinfo.c
      ${_THEORA_DIR}/decode.c
      ${_THEORA_DIR}/dequant.c
      ${_THEORA_DIR}/fragment.c
      ${_THEORA_DIR}/huffdec.c
      ${_THEORA_DIR}/idct.c
      ${_THEORA_DIR}/info.c
      ${_THEORA_DIR}/internal.c
      ${_THEORA_DIR}/quant.c
      ${_THEORA_DIR}/state.c
    )
    target_include_directories(theora_vendored PUBLIC ${_THEORA_DIR} ${_OGG_INC})
    target_compile_definitions(theora_vendored PRIVATE THEORA_DISABLE_ENCODE)
    target_compile_options(theora_vendored PRIVATE -w)
    if(EXISTS "${_THEORA_DIR}/encoder_disabled.c")
      target_sources(theora_vendored PRIVATE ${_THEORA_DIR}/encoder_disabled.c)
    endif()
  endif()
  target_link_libraries(oa_deps INTERFACE theora_vendored)
  # Static vorbis/theora need ogg after them for --as-needed.
  oa_link_prebuilt(oa_deps ogg)
  target_link_libraries(oa_deps INTERFACE EGL GLESv3 m dl hilog_ndk.z)
else()
  include(FindPkgConfig)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  pkg_check_modules(FREETYPE REQUIRED freetype2)
  pkg_check_modules(PNG REQUIRED libpng)
  pkg_check_modules(JPEG REQUIRED libjpeg)
  pkg_check_modules(VORBIS REQUIRED vorbis vorbisfile)
  pkg_check_modules(THEORA REQUIRED theora theoradec)
  pkg_check_modules(OGG REQUIRED ogg)
  pkg_check_modules(AVCODEC REQUIRED libavcodec)
  pkg_check_modules(AVFORMAT REQUIRED libavformat)
  pkg_check_modules(AVUTIL REQUIRED libavutil)
  pkg_check_modules(SWSCALE REQUIRED libswscale)
  pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

  # SDL_MAIN_HANDLED: do not pull SDL2main's WinMain / SDL_main wrapper.
  set(_OA_SDL2_LIBS ${SDL2_LIBRARIES})
  list(FILTER _OA_SDL2_LIBS EXCLUDE REGEX "SDL2main")

  target_include_directories(oa_deps INTERFACE
    ${OA_SDL2_BRIDGE_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS}
    ${VORBIS_INCLUDE_DIRS}
    ${THEORA_INCLUDE_DIRS}
    ${OGG_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
  )
  target_link_directories(oa_deps INTERFACE
    ${SDL2_LIBRARY_DIRS}
    ${FREETYPE_LIBRARY_DIRS}
    ${PNG_LIBRARY_DIRS}
    ${JPEG_LIBRARY_DIRS}
    ${VORBIS_LIBRARY_DIRS}
    ${THEORA_LIBRARY_DIRS}
    ${OGG_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${SWRESAMPLE_LIBRARY_DIRS}
  )
  target_link_libraries(oa_deps INTERFACE
    ${_OA_SDL2_LIBS}
    ${FREETYPE_LIBRARIES}
    ${PNG_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${VORBIS_LIBRARIES}
    ${THEORA_LIBRARIES}
    ${OGG_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
  )
  target_compile_definitions(oa_deps INTERFACE OA_HAVE_FFMPEG=1)
  if(WIN32)
    target_link_libraries(oa_deps INTERFACE
      opengl32 ole32 oleaut32 imm32 winmm version setupapi rpcrt4 advapi32 shell32 uuid)
  endif()
endif()

target_link_libraries(oa_deps INTERFACE lua51 physfs_vendored)
