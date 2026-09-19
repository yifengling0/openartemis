if(OHOS OR OA_HARMONY_LIB)
  set(OA_HOST_PLATFORM platform/platform_harmony.cpp)
  add_library(openartemis SHARED
    main.cpp
    harmony/oa_harmony.cpp
    ${OA_HOST_PLATFORM})
  set_target_properties(openartemis PROPERTIES
    OUTPUT_NAME "openartemis"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  target_compile_definitions(openartemis PRIVATE OA_HARMONY_LIB=1)
else()
  set(OA_HOST_PLATFORM platform/platform_desktop.cpp)
  add_executable(openartemis main.cpp ${OA_HOST_PLATFORM})
endif()

target_include_directories(openartemis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openartemis PRIVATE libopenartemis oa_deps)

if(NOT OHOS AND NOT OA_HARMONY_LIB AND OA_ENABLE_TESTS)
  add_executable(openartemis_test main_test.cpp app_test_drive.cpp ${OA_HOST_PLATFORM})
  target_include_directories(openartemis_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(openartemis_test PRIVATE libopenartemis oa_deps)
endif()

if(MSVC)
  target_compile_options(openartemis PRIVATE /W4)
else()
  target_compile_options(openartemis PRIVATE -Wall -Wextra)
endif()

if(OHOS)
  set(OA_PATCH_PYTHON "")
  foreach(_python_candidate
      "${CMAKE_SOURCE_DIR}/../../../.venv/Scripts/python.exe"
      "${CMAKE_SOURCE_DIR}/../../../venv_for_build/Scripts/python.exe"
      "${CMAKE_SOURCE_DIR}/../../.venv/Scripts/python.exe"
      "${CMAKE_SOURCE_DIR}/../../venv_for_build/Scripts/python.exe")
    if(EXISTS "${_python_candidate}")
      set(OA_PATCH_PYTHON "${_python_candidate}")
      break()
    endif()
  endforeach()
  if(NOT OA_PATCH_PYTHON)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(OA_PATCH_PYTHON "${Python3_EXECUTABLE}")
  endif()
  set(OA_SONAME_PATCHER "${CMAKE_SOURCE_DIR}/../../../patch_elf_soname.py")
  if(NOT EXISTS "${OA_SONAME_PATCHER}")
    set(OA_SONAME_PATCHER "${CMAKE_SOURCE_DIR}/../../patch_elf_soname.py")
  endif()
  if(NOT EXISTS "${OA_SONAME_PATCHER}")
    message(FATAL_ERROR "SONAME patcher not found (tried MyProject and vintage-pomelo roots)")
  endif()
  add_custom_command(TARGET openartemis POST_BUILD
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libfreetype.so.6" "libfreetype.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libavcodec.so.61" "libavcodec.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libavformat.so.61" "libavformat.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libavutil.so.59" "libavutil.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libswscale.so.8" "libswscale.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libswresample.so.5" "libswresample.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libpng16.so.16" "libpng16.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libjpeg.so.62" "libjpeg.so"
    COMMAND "${OA_PATCH_PYTHON}" "${OA_SONAME_PATCHER}" "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>" "libturbojpeg.so.0" "libturbojpeg.so"
    COMMENT "Normalizing Harmony DT_NEEDED names for libopenartemis.so"
    VERBATIM)
  # 出厂 strip：.so 从 ~24MB 降到 ~7MB（dlopen 更快）；未裁剪副本留在
  # libopenartemis.unstripped.so 供崩溃符号化。
  get_filename_component(_oa_ndk_llvm_bin "${CMAKE_TOOLCHAIN_FILE}/../../llvm/bin" ABSOLUTE)
  find_program(OA_LLVM_STRIP NAMES llvm-strip.exe llvm-strip HINTS "${_oa_ndk_llvm_bin}")
  if(OA_LLVM_STRIP)
    add_custom_command(TARGET openartemis POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:openartemis>" "$<TARGET_FILE:openartemis>.unstripped"
      COMMAND "${OA_LLVM_STRIP}" --strip-unneeded "$<TARGET_FILE:openartemis>"
      COMMENT "Stripping libopenartemis.so (unstripped copy kept alongside)"
      VERBATIM)
  else()
    message(WARNING "llvm-strip not found near ${CMAKE_TOOLCHAIN_FILE}; shipping unstripped .so")
  endif()
endif()
