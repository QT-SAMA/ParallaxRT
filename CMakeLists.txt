cmake_minimum_required(VERSION 3.24)

project(ParallaxRT VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PARALLAXRT_ENABLE_DIRECTML "Build with ONNX Runtime DirectML support" ON)
option(PARALLAXRT_ENABLE_CUDA "Build with ONNX Runtime CUDA and CUDA DIBR support" OFF)
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime C/C++ package root")
set(CUDNN_ROOT "$ENV{CUDNN_ROOT}" CACHE PATH "cuDNN package root")

if(PARALLAXRT_ENABLE_CUDA AND PARALLAXRT_ENABLE_DIRECTML)
    message(FATAL_ERROR
        "Use one ONNX Runtime package per build: set PARALLAXRT_ENABLE_DIRECTML=OFF when enabling PARALLAXRT_ENABLE_CUDA.")
endif()

if(PARALLAXRT_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
endif()

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)

find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES include build/native/include buildTransitive/native/include
    REQUIRED
)
find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES lib lib/x64 runtimes/win-x64/native build/native build/native/lib/x64
    REQUIRED
)

add_library(parallaxrt_core
    src/core/depth_estimator.cpp
    src/core/dibr_renderer.cpp
)
target_include_directories(parallaxrt_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}"
)
target_link_libraries(parallaxrt_core PUBLIC ${OpenCV_LIBS} "${ONNXRUNTIME_LIBRARY}")
target_compile_definitions(parallaxrt_core PRIVATE NOMINMAX)

if(PARALLAXRT_ENABLE_CUDA)
    target_sources(parallaxrt_core PRIVATE src/core/dibr_renderer_cuda.cu)
    target_link_libraries(parallaxrt_core PUBLIC CUDA::cudart)
    target_compile_definitions(parallaxrt_core PUBLIC PARALLAXRT_ENABLE_CUDA=1)
    set_target_properties(parallaxrt_core PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "61;75;86;89;89-real;90-virtual"
    )
endif()

if(PARALLAXRT_ENABLE_DIRECTML)
    if(NOT WIN32)
        message(FATAL_ERROR "DirectML execution provider is available only on Windows.")
    endif()
    find_path(DIRECTML_PROVIDER_INCLUDE_DIR
        NAMES dml_provider_factory.h
        HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
        PATH_SUFFIXES
            include/onnxruntime/core/providers/dml
            build/native/include/onnxruntime/core/providers/dml
            build/native/include
            include
        REQUIRED
    )
    target_include_directories(parallaxrt_core PRIVATE "${DIRECTML_PROVIDER_INCLUDE_DIR}")
    target_compile_definitions(parallaxrt_core PRIVATE PARALLAXRT_ENABLE_DIRECTML=1)
endif()

if(WIN32)
    # DirectShow Transform Filter (.ax)
    add_library(parallaxrt_filter SHARED
        src/core/directshow_filter.cpp
        src/core/directshow_exports.cpp
        src/core/parallaxrt_filter.def
        src/core/parallaxrt.rc
    )
    set_target_properties(parallaxrt_filter PROPERTIES
        OUTPUT_NAME "ParallaxRTFilter"
        SUFFIX ".ax"
    )
    target_link_libraries(parallaxrt_filter PRIVATE parallaxrt_core strmiids ole32 oleaut32 advapi32 comctl32)
    target_compile_definitions(parallaxrt_filter PRIVATE NOMINMAX UNICODE _UNICODE)
endif()

set(ALL_APP_TARGETS parallaxrt_filter)

set(DEPTH_MODELS
    "${CMAKE_CURRENT_SOURCE_DIR}/Depth Anything V2 (Small - ONNX).onnx"
    "${CMAKE_CURRENT_SOURCE_DIR}/Depth Anything V2 (Base - ONNX).onnx"
)
foreach(depth_model IN LISTS DEPTH_MODELS)
    if(EXISTS "${depth_model}")
        foreach(app_target IN LISTS ALL_APP_TARGETS)
            if(TARGET ${app_target})
                add_custom_command(TARGET ${app_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${depth_model}"
                        "$<TARGET_FILE_DIR:${app_target}>"
                )
            endif()
        endforeach()
    endif()
endforeach()

if(MSVC)
    target_compile_options(parallaxrt_core PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>
    )
    if(TARGET parallaxrt_filter)
        target_compile_options(parallaxrt_filter PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()

if(WIN32 AND ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_RUNTIME_NAMES onnxruntime.dll onnxruntime_providers_shared.dll)
    if(PARALLAXRT_ENABLE_CUDA)
        list(APPEND ONNXRUNTIME_RUNTIME_NAMES onnxruntime_providers_cuda.dll)
    elseif(PARALLAXRT_ENABLE_DIRECTML)
        list(APPEND ONNXRUNTIME_RUNTIME_NAMES onnxruntime_providers_dml.dll DirectML.dll)
    endif()
    foreach(runtime_name IN LISTS ONNXRUNTIME_RUNTIME_NAMES)
        string(MAKE_C_IDENTIFIER "${runtime_name}" runtime_variable)
        find_file("${runtime_variable}_PATH"
            NAMES "${runtime_name}"
            HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/deps/microsoft.ai.directml-1.15.4"
                  "$ENV{WINDIR}/System32"
            PATH_SUFFIXES lib bin bin/x64-win runtimes/win-x64/native build/native build/native/bin
        )
        if(DEFINED ${runtime_variable}_PATH AND
           NOT "${${runtime_variable}_PATH}" MATCHES "-NOTFOUND$")
            foreach(app_target IN LISTS ALL_APP_TARGETS)
                if(TARGET ${app_target})
                    add_custom_command(TARGET ${app_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${${runtime_variable}_PATH}"
                            "$<TARGET_FILE_DIR:${app_target}>"
                    )
                endif()
            endforeach()
        endif()
    endforeach()
endif()

if(WIN32 AND PARALLAXRT_ENABLE_CUDA)
    set(CUDNN_BIN_DIR "${CUDNN_ROOT}/nvidia/cudnn/bin")
    file(GLOB CUDNN_RUNTIME_DLLS CONFIGURE_DEPENDS "${CUDNN_BIN_DIR}/*.dll")
    list(FILTER CUDNN_RUNTIME_DLLS EXCLUDE REGEX "cudnn_adv.*\\.dll$")
    if(NOT CUDNN_RUNTIME_DLLS)
        message(FATAL_ERROR
            "CUDA builds require cuDNN 9 runtime DLLs. Set CUDNN_ROOT to the extracted cuDNN package root.")
    endif()
    foreach(cudnn_runtime_dll IN LISTS CUDNN_RUNTIME_DLLS)
        foreach(app_target IN LISTS ALL_APP_TARGETS)
            if(TARGET ${app_target})
                add_custom_command(TARGET ${app_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${cudnn_runtime_dll}"
                        "$<TARGET_FILE_DIR:${app_target}>"
                )
            endif()
        endforeach()
    endforeach()

    find_file(CUFFT_RUNTIME_DLL
        NAMES cufft64_11.dll
        HINTS "${CMAKE_CURRENT_SOURCE_DIR}/deps"
              "${CUDAToolkit_ROOT}/bin"
              "$ENV{CUDA_PATH}/bin"
    )
    if(DEFINED CUFFT_RUNTIME_DLL AND NOT "${CUFFT_RUNTIME_DLL}" MATCHES "-NOTFOUND$")
        foreach(app_target IN LISTS ALL_APP_TARGETS)
            if(TARGET ${app_target})
                add_custom_command(TARGET ${app_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CUFFT_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:${app_target}>"
                )
            endif()
        endforeach()
    endif()

    find_file(CUDART_RUNTIME_DLL
        NAMES cudart64_12.dll
        HINTS "${CMAKE_CURRENT_SOURCE_DIR}/deps"
              "${CUDAToolkit_ROOT}/bin"
              "$ENV{CUDA_PATH}/bin"
    )
    if(DEFINED CUDART_RUNTIME_DLL AND NOT "${CUDART_RUNTIME_DLL}" MATCHES "-NOTFOUND$")
        foreach(app_target IN LISTS ALL_APP_TARGETS)
            if(TARGET ${app_target})
                add_custom_command(TARGET ${app_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CUDART_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:${app_target}>"
                )
            endif()
        endforeach()
    endif()

    set(CUDA_EXTRA_RUNTIME_NAMES cublas64_12.dll cublasLt64_12.dll)
    foreach(cuda_dll_name IN LISTS CUDA_EXTRA_RUNTIME_NAMES)
        string(MAKE_C_IDENTIFIER "${cuda_dll_name}" cuda_dll_var)
        find_file("${cuda_dll_var}_PATH"
            NAMES "${cuda_dll_name}"
            HINTS "${CMAKE_CURRENT_SOURCE_DIR}/deps"
                  "${CUDAToolkit_ROOT}/bin"
                  "$ENV{CUDA_PATH}/bin"
        )
        if(DEFINED ${cuda_dll_var}_PATH AND NOT "${${cuda_dll_var}_PATH}" MATCHES "-NOTFOUND$")
            foreach(app_target IN LISTS ALL_APP_TARGETS)
                if(TARGET ${app_target})
                    add_custom_command(TARGET ${app_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${${cuda_dll_var}_PATH}"
                            "$<TARGET_FILE_DIR:${app_target}>"
                    )
                endif()
            endforeach()
        endif()
    endforeach()
endif()
