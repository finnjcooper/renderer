cmake_minimum_required(VERSION 3.12)
project(main)

set(CMAKE_CXX_STANDARD 14)

include_directories(libs/glm-0.9.7.2)
include_directories(libs/sdw)
include_directories(libs/extra)

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
        src/main.cpp
)

target_compile_options(main
    PUBLIC
    -Wall
    -Wextra
    -Wcast-align
    -Wfatal-errors
    -Werror=return-type
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-ignored-attributes)

set(DEBUG_OPTIONS -Og -fno-omit-frame-pointer -g)
set(RELEASE_OPTIONS -O3 -march=native -mtune=native)
set(SPEEDY_OPTIONS -Ofast -funsafe-math-optimizations -march=native -mtune=native)
target_link_libraries(main PUBLIC $<$<CONFIG:Debug>:-Wl,-lasan>)

target_compile_options(main PUBLIC "$<$<CONFIG:Release>:${RELEASE_OPTIONS}>")
target_compile_options(main PUBLIC "$<$<CONFIG:Debug>:${DEBUG_OPTIONS}>")
target_compile_options(main PUBLIC "$<$<CONFIG:Speedy>:${SPEEDY_OPTIONS}>")
