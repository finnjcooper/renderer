build:
    emcmake cmake -S . -B build -GNinja
    cmake --build build

clean:
    cmake -E remove_directory build
