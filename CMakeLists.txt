cmake_minimum_required(VERSION 3.12)
project(main)

set(CMAKE_CXX_STANDARD 20)

include_directories(libs/glm-0.9.7.2)
include_directories(libs/sdw)
include_directories(libs/extra)
include_directories(libs/webgpu)

add_executable(main
        libs/sdw/CanvasPoint.cpp
        libs/sdw/CanvasTriangle.cpp
        libs/sdw/CanvasLine.cpp
        libs/sdw/DrawingWindow.cpp
        libs/sdw/ModelTriangle.cpp
        libs/sdw/HitInfo.cpp
        libs/sdw/TextureMap.cpp
        libs/sdw/TexturePoint.cpp
        libs/sdw/Utils.cpp
        libs/extra/Camera.cpp
        libs/extra/Material.cpp
        libs/extra/Renderer.cpp
        libs/extra/Scene.cpp
        libs/extra/Vertex.cpp
        libs/extra/Material.cpp
        libs/extra/Ray.cpp
        libs/extra/ExtraUtils.cpp
        libs/extra/Light.cpp
        libs/extra/Obj.cpp
        libs/extra/Model.cpp
        libs/extra/CubeMap.cpp
        libs/webgpu/WebGPURenderer.cpp
        src/main.cpp
)

target_compile_options(main
    PUBLIC
    -O3
    -Wall
    -Wextra
    -Wcast-align
    -Wfatal-errors
    -Werror=return-type
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-ignored-attributes
    "--use-port=emdawnwebgpu")

set_target_properties(main PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")

target_link_options(main PUBLIC
    "-O3"
    "-sALLOW_MEMORY_GROWTH=1"
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
    "--use-port=emdawnwebgpu"
    "--closure=1"
    "-sASYNCIFY=1"
)
