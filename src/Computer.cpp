/*
 * Computer.cpp
 * CraftOS-PC 2
 * 
 * This file implements the methods of the Computer class.
 * 
 * This code is licensed under the MIT license.
 * Copyright (c) 2019-2020 JackMacWindows.
 */

#define CRAFTOSPC_INTERNAL
#include "Computer.hpp"
#include "platform.hpp"
#include "term.hpp"
#include "bit.hpp"
#include "config.hpp"
#include "fs.hpp"
#include "fs_standalone.hpp"
#include "http.hpp"
#include "mounter.hpp"
#include "os.hpp"
#include "redstone.hpp"
#include "peripheral/peripheral.hpp"
#include "peripheral/computer.hpp"
#include "terminal/SDLTerminal.hpp"
#include "terminal/CLITerminal.hpp"
#include "terminal/RawTerminal.hpp"
#include "terminal/TRoRTerminal.hpp"
#include "periphemu.hpp"
#include <unordered_set>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>

#define PLUGIN_VERSION 2

extern std::string asciify(std::string);
extern int selectedRenderer;
extern bool forceCheckTimeout;
extern std::string script_args;
extern std::string script_file;
std::vector<Computer*> computers;
std::unordered_set<Computer*> freedComputers; 
std::unordered_set<SDL_TimerID> freedTimers;
std::mutex freedTimersMutex;

// Basic CraftOS libraries
library_t * libraries[] = {
    &bit_lib,
    &config_lib,
    &fs_lib,
    &mounter_lib,
    &os_lib,
    &peripheral_lib,
    &periphemu_lib,
    &rs_lib,
    &term_lib
};

// Constructor
Computer::Computer(int i, bool debug): isDebugger(debug) {
    id = i;
    // Tell the mounter it's initializing to prevent checking rom remounts
    mounter_initializing = true;
#ifdef STANDALONE_ROM
    addMount(this, "rom:", "rom", true);
    if (debug) addMount(this, "debug:", "debug", true);
#else
    if (!addMount(this, (getROMPath() + "/rom").c_str(), "rom", ::config.romReadOnly)) throw std::runtime_error("Could not mount ROM");
    if (debug) if (!addMount(this, (getROMPath() + "/debug").c_str(), "debug", true)) throw std::runtime_error("Could not mount debugger ROM");
#endif
    mounter_initializing = false;
    // Create the root directory
#ifdef _WIN32
    createDirectory((std::string(getBasePath()) + "\\computer\\" + std::to_string(id)).c_str());
#else
    createDirectory((std::string(getBasePath()) + "/computer/" + std::to_string(id)).c_str());
#endif
    // Load config
    config = getComputerConfig(id);
    // Create the terminal
    std::string term_title = config.label.empty() ? "CraftOS Terminal: " + std::string(debug ? "Debugger" : "Computer") + " " + std::to_string(id) : "CraftOS Terminal: " + asciify(config.label);
    if (selectedRenderer == 1) term = NULL;
#ifndef NO_CLI
    else if (selectedRenderer == 2) term = new CLITerminal(term_title);
#endif
    else if (selectedRenderer == 3) term = new RawTerminal(term_title);
    else if (selectedRenderer == 4) term = new TRoRTerminal(term_title);
    else term = new SDLTerminal(term_title);
}

extern void stopWebsocket(void*);

// Destructor
Computer::~Computer() {
    // Destroy terminal
    if (term != NULL) delete term;
    // Save config
    setComputerConfig(id, config);
    // Deinitialize all peripherals
    for (auto p : peripherals) p.second->getDestructor()(p.second);
    for (std::list<Computer*>::iterator c = referencers.begin(); c != referencers.end(); c++) {
        std::lock_guard<std::mutex> lock((*c)->peripherals_mutex);
        for (auto it = (*c)->peripherals.begin(); it != (*c)->peripherals.end(); it++) {
            if (std::string(it->second->getMethods().name) == "computer" && ((computer*)it->second)->comp == this) {
                // Detach computer peripherals pointing to this on other computers
                delete (computer*)it->second;
                it = (*c)->peripherals.erase(it);
                if (it == (*c)->peripherals.end()) break;
            }
        }
        if (c == referencers.end()) break;
    }
	// Mark all currently running timers as invalid
	{
		std::lock_guard<std::mutex> lock(freedTimersMutex);
		for (SDL_TimerID t : timerIDs) freedTimers.insert(t);
	}
    // Cancel the mouse_move debounce timer if active
    if (mouseMoveDebounceTimer != 0) SDL_RemoveTimer(mouseMoveDebounceTimer);
    if (eventTimeout != 0) SDL_RemoveTimer(eventTimeout);
    // Stop all open websockets
    while (openWebsockets.size() > 0) {
        void* it = *openWebsockets.begin();
        stopWebsocket(it);
    }
}

extern int fs_getName(lua_State *L);

extern "C" {
    extern int db_errorfb (lua_State *L);

    int db_breakpoint(lua_State *L) {
        if (!lua_isstring(L, 1)) bad_argument(L, "string", 1);
        if (!lua_isnumber(L, 2)) bad_argument(L, "number", 2);
        Computer * computer = get_comp(L);
        int id = computer->breakpoints.size() > 0 ? computer->breakpoints.rbegin()->first + 1 : 1;
        computer->breakpoints[id] = std::make_pair("@/" + fixpath(computer, lua_tostring(L, 1), false, false), lua_tointeger(L, 2));
        if (!computer->hasBreakpoints) forceCheckTimeout = true;
        computer->hasBreakpoints = true;
        lua_sethook(computer->L, termHook, LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
        lua_sethook(L, termHook, LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
        lua_pushinteger(L, id);
        return 1;
    }

    int db_unsetbreakpoint(lua_State *L) {
        if (!lua_isnumber(L, 1)) bad_argument(L, "number", 1);
        Computer * computer = get_comp(L);
        if (computer->breakpoints.find(lua_tointeger(L, 1)) != computer->breakpoints.end()) {
            computer->breakpoints.erase(lua_tointeger(L, 1));
            if (computer->breakpoints.size() == 0) {
                computer->hasBreakpoints = false;
                lua_sethook(computer->L, termHook, LUA_MASKCOUNT | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
                lua_sethook(L, termHook, LUA_MASKCOUNT | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
            }
            lua_pushboolean(L, true);
        } else lua_pushboolean(L, false);
        return 1;
    }

    void setcompmask(lua_State *L, int mask) {
        Computer * comp = get_comp(L);
        comp->hookMask = mask;
    }
}

library_t * getLibrary(std::string name) {
    if (name == "bit") return &bit_lib;
    else if (name == "config") return &config_lib;
    else if (name == "fs") return &fs_lib; 
    else if (name == "mounter") return &mounter_lib; 
    else if (name == "os") return &os_lib; 
    else if (name == "peripheral") return &peripheral_lib; 
    else if (name == "periphemu") return &periphemu_lib; 
    else if (name == "redstone" || name == "rs") return &rs_lib; 
    else if (name == "term") return &term_lib; 
    else return NULL;
}

static void pluginError(lua_State *L, const char * name, const char * err) {
    lua_getglobal(L, "_CCPC_PLUGIN_ERRORS");
    if (lua_isnil(L, -1)) {
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_CCPC_PLUGIN_ERRORS");
    }
    lua_pushstring(L, name);
    lua_pushstring(L, err);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

// Main computer loop
void Computer::run(std::string bios_name) {
    running = 1;
    if (L != NULL) lua_close(L);
    setjmp(on_panic);
    while (running) {
        int status;
        if (term != NULL) {
            // Initialize terminal contents
            std::lock_guard<std::mutex> lock(term->locked);
            term->blinkX = 0;
            term->blinkY = 0;
            term->screen = vector2d<unsigned char>(term->width, term->height, ' ');
            term->colors = vector2d<unsigned char>(term->width, term->height, 0xF0);
            term->pixels = vector2d<unsigned char>(term->width * Terminal::fontWidth, term->height * Terminal::fontHeight, 0x0F);
            memcpy(term->palette, defaultPalette, sizeof(defaultPalette));
            term->mode = 0;
        }
        colors = 0xF0;

        /*
        * All Lua contexts are held in this structure. We work with it almost
        * all the time.
        */
        L = luaL_newstate();

        coro = lua_newthread(L);
        paramQueue = lua_newthread(L);
        while (!eventQueue.empty()) eventQueue.pop();

        // Push reference to this to the registry
        //lua_pushlightuserdata(L, &computer_key);
        lua_pushinteger(L, 1);
        lua_pushlightuserdata(L, this);
        lua_settable(L, LUA_REGISTRYINDEX);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "_coroutine_stack");

        // Load libraries
        luaL_openlibs(coro);
        lua_getglobal(L, "os");
        lua_getfield(L, -1, "date");
        lua_setglobal(L, "os_date");
        lua_pop(L, 1);
        if (debugger != NULL && !isDebugger) lua_sethook(coro, termHook, LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
        else if (::config.debug_enable && !isDebugger) lua_sethook(coro, termHook, LUA_MASKCOUNT | LUA_MASKRET | LUA_MASKCALL | LUA_MASKERROR | LUA_MASKRESUME | LUA_MASKYIELD, 1000000);
        else lua_sethook(coro, termHook, LUA_MASKCOUNT | LUA_MASKERROR, 1000000);
        lua_atpanic(L, termPanic);
        for (unsigned i = 0; i < sizeof(libraries) / sizeof(library_t*); i++) load_library(this, coro, *libraries[i]);
        if (::config.http_enable) load_library(this, coro, http_lib);
        if (isDebugger) load_library(this, coro, *((library_t*)debugger));
        lua_getglobal(coro, "redstone");
        lua_setglobal(coro, "rs");
        lua_getglobal(L, "os");
        lua_getglobal(L, "os_date");
        lua_setfield(L, -2, "date");
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_setglobal(L, "os_date");

        // Load any plugins available
#ifndef STANDALONE_ROM
        if (!::config.vanilla) {
            lua_newtable(L);
            lua_setfield(L, LUA_REGISTRYINDEX, "plugin_info");
            struct dirent *dir;
            std::string plugin_path = getPlugInPath();
            DIR * d = opendir(plugin_path.c_str());
            struct stat st;
            if (d) {
                for (int i = 0; (dir = readdir(d)) != NULL; i++) {
                    if (stat((plugin_path + "/" + std::string(dir->d_name)).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
                    if (std::string(dir->d_name) == ".DS_Store" || std::string(dir->d_name) == "desktop.ini") continue;
                    std::string api_name = std::string(dir->d_name).substr(0, std::string(dir->d_name).find_first_of('.'));
                    lua_CFunction info = (lua_CFunction)loadSymbol(plugin_path + "/" + dir->d_name, "plugin_info");
                    if (info == NULL) {
                        printf("The plugin \"%s\" is not verified to work with CraftOS-PC. Use at your own risk.\n", api_name.c_str()); 
                        pluginError(L, api_name.c_str(), "Missing plugin info");
                    } else {
                        lua_pushcfunction(L, info);
                        lua_call(L, 0, 1);
                        if (!lua_istable(L, -1)) {
                            printf("The plugin \"%s\" returned invalid info. Use at your own risk.", api_name.c_str());
                            pluginError(L, api_name.c_str(), "Invalid plugin info");
                        } else {
                            lua_getfield(L, LUA_REGISTRYINDEX, "plugin_info");
                            lua_pushvalue(L, -2);
                            lua_setfield(L, -2, api_name.c_str());
                            lua_pop(L, 1);
                            
                            lua_getfield(L, -1, "version");
                            if (!lua_isnumber(L, -1) || lua_tointeger(L, -1) < PLUGIN_VERSION) {
                                printf("The plugin \"%s\" is built for an older version of CraftOS-PC (%td). Use at your own risk.\n", api_name.c_str(), lua_tointeger(L, -1));
                                pluginError(L, api_name.c_str(), "Old plugin API");
                            }
                            lua_pop(L, 1);

                            lua_getfield(L, -1, "register_getLibrary");
                            if (lua_isfunction(L, -1)) {
                                lua_pushlightuserdata(L, (void*)&getLibrary);
                                lua_call(L, 1, 0);
                            } else lua_pop(L, 1);
                            
                            lua_getfield(L, -1, "register_registerPeripheral");
                            if (lua_isfunction(L, -1)) {
                                lua_pushlightuserdata(L, (void*)&registerPeripheral);
                                lua_call(L, 1, 0);
                            } else lua_pop(L, 1);

                            lua_getfield(L, -1, "register_addMount");
                            if (lua_isfunction(L, -1)) {
                                lua_pushlightuserdata(L, (void*)&addMount);
                                lua_call(L, 1, 0);
                            } else lua_pop(L, 1);

                            lua_getfield(L, -1, "register_termQueueProvider");
                            if (lua_isfunction(L, -1)) {
                                lua_pushlightuserdata(L, (void*)&termQueueProvider);
                                lua_call(L, 1, 0);
                            } else lua_pop(L, 1);

                            lua_getfield(L, -1, "register_startComputer");
                            if (lua_isfunction(L, -1)) {
                                lua_pushlightuserdata(L, (void*)&startComputer);
                                lua_call(L, 1, 0);
                            } else lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                    lua_CFunction luaopen = (lua_CFunction)loadSymbol(plugin_path + "/" + dir->d_name, "luaopen_" + api_name);
                    if (luaopen == NULL) {
                        printf("Error loading plugin %s: %s\n", api_name.c_str(), lua_tostring(L, -1)); 
                        pluginError(L, api_name.c_str(), "Missing API opener");
                        continue;
                    }
                    lua_pushcfunction(L, luaopen);
                    lua_pushstring(L, getROMPath().c_str());
                    lua_pushstring(L, getBasePath().c_str());
                    lua_call(L, 2, 1);
                    lua_setglobal(L, api_name.c_str());
                }
                closedir(d);
            } //else printf("Could not open plugins from %s\n", plugin_path.c_str());
        }
#endif

        // Delete unwanted globals
        lua_pushnil(L);
        lua_setglobal(L, "dofile");
        lua_pushnil(L);
        lua_setglobal(L, "loadfile");
        lua_pushnil(L);
        lua_setglobal(L, "module");
        lua_pushnil(L);
        lua_setglobal(L, "require");
        lua_pushnil(L);
        lua_setglobal(L, "package");
        lua_pushnil(L);
        lua_setglobal(L, "print");
        if (!::config.debug_enable) {
            lua_pushnil(L);
            lua_setglobal(L, "collectgarbage");
            lua_pushnil(L);
            lua_setglobal(L, "debug");
            lua_pushnil(L);
            lua_setglobal(L, "newproxy");
        }
        if (::config.vanilla) {
            lua_pushnil(L);
            lua_setglobal(L, "config");
            lua_pushnil(L);
            lua_setglobal(L, "mounter");
            lua_pushnil(L);
            lua_setglobal(L, "periphemu");
            lua_getglobal(L, "term");
            lua_pushnil(L);
            lua_setfield(L, -2, "getGraphicsMode");
            lua_pushnil(L);
            lua_setfield(L, -2, "setGraphicsMode");
            lua_pushnil(L);
            lua_setfield(L, -2, "getPixel");
            lua_pushnil(L);
            lua_setfield(L, -2, "setPixel");
            lua_pushnil(L);
            lua_setfield(L, -2, "drawPixels");
            lua_pushnil(L);
            lua_setfield(L, -2, "screenshot");
            lua_pop(L, 1);
            if (::config.http_enable) {
                lua_getglobal(L, "http");
                lua_pushnil(L);
                lua_setfield(L, -2, "addListener");
                lua_pushnil(L);
                lua_setfield(L, -2, "removeListener");
                lua_pop(L, 1);
            }
            if (::config.debug_enable) {
                lua_getglobal(L, "debug");
                lua_pushnil(L);
                lua_setfield(L, -2, "setbreakpoint");
                lua_pushnil(L);
                lua_setfield(L, -2, "unsetbreakpoint");
                lua_pop(L, 1);
            }
        }

        // Set default globals
        lua_pushstring(L, ::config.default_computer_settings.c_str());
        lua_setglobal(L, "_CC_DEFAULT_SETTINGS");
        lua_pushboolean(L, ::config.disable_lua51_features);
        lua_setglobal(L, "_CC_DISABLE_LUA51_FEATURES");
#if CRAFTOSPC_INDEV == true && defined(CRAFTOSPC_COMMIT)
        lua_pushstring(L, "ComputerCraft 1.87.1 (CraftOS-PC " CRAFTOSPC_COMMIT ")");
#else
        lua_pushstring(L, "ComputerCraft 1.87.1 (CraftOS-PC " CRAFTOSPC_VERSION ")");
#endif
        lua_setglobal(L, "_HOST");
        if (selectedRenderer == 1) {
            lua_pushboolean(L, true);
            lua_setglobal(L, "_HEADLESS");
        }
        if (!script_file.empty()) {
            FILE* in = fopen(script_file.c_str(), "r");
            std::string script;
            char tmp[4096];
            while (!feof(in)) {
                size_t read = fread(tmp, 1, 4096, in);
                if (read == 0) break;
                script += std::string(tmp, read);
            }
            fclose(in);
            lua_pushlstring(L, script.c_str(), script.size());
            lua_setglobal(L, "_CCPC_STARTUP_SCRIPT");
        }
        if (!script_args.empty()) {
            lua_pushlstring(L, script_args.c_str(), script_args.length());
            lua_setglobal(L, "_CCPC_STARTUP_ARGS");
        }

        /* Load the file containing the script we are going to run */
#ifdef STANDALONE_ROM
        status = luaL_loadstring(coro, bios_name.c_str());
        std::string bios_path_expanded = "standalone ROM";
#else
#ifdef WIN32
        std::string bios_path_expanded = getROMPath() + "\\" + bios_name;
#else
        std::string bios_path_expanded = getROMPath() + "/" + bios_name;
#endif
        status = luaL_loadfile(coro, bios_path_expanded.c_str());
#endif
        if (status || !lua_isfunction(coro, -1)) {
            /* If something went wrong, error message is at the top of */
            /* the stack */
            fprintf(stderr, "Couldn't load BIOS: %s (%s). Please make sure the CraftOS ROM is installed properly. (See https://www.craftos-pc.cc/docs/error-messages for more information.)\n", bios_path_expanded.c_str(), lua_tostring(L, -1));
            queueTask([bios_path_expanded](void* term)->void*{
                ((Terminal*)term)->showMessage(
                    SDL_MESSAGEBOX_ERROR, "Couldn't load BIOS", 
                    std::string(
                        "Couldn't load BIOS from " + bios_path_expanded + ". Please make sure the CraftOS ROM is installed properly. (See https://www.craftos-pc.cc/docs/error-messages for more information.)"
                    ).c_str()
                ); 
                return NULL;
            }, term);
            return;
        }

        /* Ask Lua to run our little script */
        status = LUA_YIELD;
        int narg = 0;
        running = 1;
        while (status == LUA_YIELD && running == 1) {
            status = lua_resume(coro, narg);
            if (status == LUA_YIELD) {
                if (lua_isstring(coro, -1)) narg = getNextEvent(coro, std::string(lua_tostring(coro, -1), lua_strlen(coro, -1)));
                else narg = getNextEvent(coro, "");
            } else if (status != 0) {
                // Catch runtime error
                running = 0;
                lua_pushcfunction(coro, termPanic);
                if (lua_isstring(coro, -2)) lua_pushvalue(coro, -2);
                else lua_pushnil(coro);
                lua_call(coro, 1, 0);
                break;
            }
        }
        
        // Shutdown threads
        event_lock.notify_all();
        for (unsigned i = 0; i < sizeof(libraries) / sizeof(library_t*); i++) 
            if (libraries[i]->deinit != NULL) libraries[i]->deinit(this);
        lua_close(L);   /* Cya, Lua */
        L = NULL;
    }
}

// Gets the next event for the given computer
bool Computer::getEvent(SDL_Event* e) {
    if (termEventQueue.size() == 0) return false;
    memcpy(e, &termEventQueue.front(), sizeof(SDL_Event));
    termEventQueue.pop();
    return true;
}

extern std::mutex taskQueueMutex;
extern std::atomic_bool taskQueueReady;
extern std::condition_variable taskQueueNotify;
extern bool exiting;

// Thread wrapper for running a computer
void* computerThread(void* data) {
    Computer * comp = (Computer*)data;
#ifdef __APPLE__
    pthread_setname_np(std::string("Computer " + std::to_string(comp->id) + " Thread").c_str());
#endif
#ifdef STANDALONE_ROM
    comp->run(standaloneBIOS);
#else
    comp->run("bios.lua");
#endif
    freedComputers.insert(comp);
    for (auto it = computers.begin(); it != computers.end(); it++) {
        if (*it == comp) {
            it = computers.erase(it);
            queueTask([](void* arg)->void* {delete (Computer*)arg; return NULL; }, comp);
            if (it == computers.end()) break;
        }
    }
    if (selectedRenderer != 0 && selectedRenderer != 2 && !exiting) {
        {std::lock_guard<std::mutex> lock(taskQueueMutex);}
        while (taskQueueReady && !exiting) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        taskQueueReady = true;
        taskQueueNotify.notify_all();
        while (taskQueueReady && !exiting) {std::this_thread::yield(); taskQueueNotify.notify_all();}
    }
    return NULL;
}

std::list<std::thread*> computerThreads;

// Spin up a new computer
Computer * startComputer(int id) {
    Computer * comp;
    try {comp = new Computer(id);} catch (std::exception &e) {
        if (selectedRenderer == 0) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Failed to open computer", std::string("An error occurred while opening the computer session: " + std::string(e.what()) + ". See https://www.craftos-pc.cc/docs/error-messages for more info.").c_str(), NULL);
        else fprintf(stderr, "An error occurred while opening the computer session: %s", e.what());
        return NULL;
    }
    computers.push_back(comp);
    std::thread * th = new std::thread(computerThread, comp);
    setThreadName(*th, std::string("Computer " + std::to_string(id) + " Thread").c_str());
    computerThreads.push_back(th);
    return comp;
}