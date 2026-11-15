clean:
    cmake -E remove_directory build

generate:
    emcmake cmake -S . -B build -GNinja

build: generate
    cmake --build build

run: build
    python -m http.server 8000 -d build
