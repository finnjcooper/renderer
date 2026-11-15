#pragma once

#include <iostream>
#include <fstream>
#include <vector>

#ifndef __EMSCRIPTEN__
#include "SDL2/SDL.h"
#else
#include <emscripten.h>
#endif

class DrawingWindow {

public:
    size_t width;
    size_t height;

private:
    std::vector<uint32_t> pixelBuffer;

#ifndef __EMSCRIPTEN__
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
#endif

public:
    DrawingWindow();
    DrawingWindow(int w, int h, bool fullscreen);
    void renderFrame();
    void savePPM(const std::string &filename) const;

#ifndef __EMSCRIPTEN__
    void saveBMP(const std::string &filename) const;
    bool pollForInputEvents(SDL_Event &event);
    void exitCleanly();
#endif

    void setPixelColour(size_t x, size_t y, uint32_t colour);
    uint32_t getPixelColour(size_t x, size_t y);
    void clearPixels();
};
