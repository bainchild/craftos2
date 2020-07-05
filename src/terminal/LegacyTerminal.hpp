/*
 * LegacyTerminal.hpp
 * CraftOS-PC 2
 * 
 * This file defines the LegacyTerminal class.
 * 
 * This code is licensed under the MIT license.
 * Copyright (c) 2019 JackMacWindows.
 */

class LegacyTerminal;
#ifndef LEGACYTERMINAL_HPP
#define LEGACYTERMINAL_HPP
#ifdef _WIN32
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include <string>
#include <vector>
#include <ctime>
#include <atomic>
#include <mutex>
#include "Terminal.hpp"
#include "../platform.hpp"

class LegacyTerminal : public Terminal {
protected:
    static int fontScale;
    bool shouldScreenshot = false;
    bool shouldRecord = false;
    bool fullscreen = false;
    std::string screenshotPath;
    std::string recordingPath;
    int recordedFrames = 0;
    int frameWait = 0;
    std::vector<std::string> recording;
    std::mutex recorderMutex;
    bool overridden = false;
    std::string title;
public:
    int charScale = 2;
    int dpiScale = 1;
    int charWidth = fontWidth * 2/fontScale * charScale;
    int charHeight = fontHeight * 2/fontScale * charScale;
    int lastFPS = 0;
    int currentFPS = 0;
    int lastSecond = time(0);

    static void init();
    static void quit();
	static bool pollEvents();
    LegacyTerminal(std::string title);
    ~LegacyTerminal() override;
    void setPalette(Color * p);
    void setCharScale(int scale);
    bool drawChar(char c, int x, int y, Color fg, Color bg, bool transparent = false);
    void render() override;
    bool resize(int w, int h) override;
    void getMouse(int *x, int *y);
    void screenshot(std::string path = ""); // asynchronous; captures on next render
    void record(std::string path = ""); // asynchronous; captures on next render
    void stopRecording();
    void toggleRecording() { if (shouldRecord) stopRecording(); else record(); }
    void showMessage(Uint32 flags, const char * title, const char * message) override;
    void toggleFullscreen();
    void setLabel(std::string label) override;

    SDL_Window *win;
private:
    SDL_Renderer *ren;
    SDL_Surface *bmp;
    SDL_Texture *font;

    static SDL_Rect getCharacterRect(char c);
};
#endif