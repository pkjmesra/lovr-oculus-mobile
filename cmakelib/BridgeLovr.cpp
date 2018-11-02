#include <stdio.h>
#include <android/log.h>
#include <string.h>
#include "physfs.h"
#include <string>
#include <sys/stat.h>
#include <assert.h>

extern "C" {

#include "BridgeLovr.h"
#include "luax.h"
#include "lib/glfw.h"

#include "api.h"
#include "lib/lua-cjson/lua_cjson.h"
#include "lib/lua-enet/enet.h"
#include "headset/oculus_mobile.h"

// Implicit from boot.lua.h
extern unsigned char boot_lua[];
extern unsigned int boot_lua_len;

static lua_State* L, *Lcoroutine;
static int coroutineRef = LUA_NOREF;
static int coroutineStartFunctionRef = LUA_NOREF;

// Exposed to oculus_mobile.c
char *bridgeLovrWritablePath;
BridgeLovrMobileData bridgeLovrMobileData;

int lovr_luaB_print_override (lua_State *L);

// Recursively copy a subdirectory out of PhysFS onto disk
static void physCopyFiles(std::string toDir, std::string fromDir) {
  char **filesOrig = PHYSFS_enumerateFiles(fromDir.c_str());
  char **files = filesOrig;

  if (!files) {
    __android_log_print(ANDROID_LOG_ERROR, "LOVR", "COULD NOT READ DIRECTORY SOMEHOW: [%s]", fromDir.c_str());
    return;
  }

  mkdir(toDir.c_str(), 0777);

  while (*files) {
    std::string fromPath = fromDir + "/" + *files;
    std::string toPath = toDir + "/" + *files;
    PHYSFS_Stat stat;
    PHYSFS_stat(fromPath.c_str(), &stat);

    if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
      __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "DIR:  [%s] INTO: [%s]", fromPath.c_str(), toPath.c_str());
      physCopyFiles(toPath, fromPath);
    } else {
      __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "FILE: [%s] INTO: [%s]", fromPath.c_str(), toPath.c_str());

      PHYSFS_File *fromFile = PHYSFS_openRead(fromPath.c_str());

      if (!fromFile) {
        __android_log_print(ANDROID_LOG_ERROR, "LOVR", "COULD NOT OPEN TO READ:  [%s]", fromPath.c_str());
      
      } else {
        FILE *toFile = fopen(toPath.c_str(), "w");

        if (!toFile) {
          __android_log_print(ANDROID_LOG_ERROR, "LOVR", "COULD NOT OPEN TO WRITE: [%s]", toPath.c_str());

        } else {
#define CPBUFSIZE (1024*8)
          while(1) {
            char buffer[CPBUFSIZE];
            int written = PHYSFS_readBytes(fromFile, buffer, CPBUFSIZE);
            if (written > 0)
              fwrite(buffer, 1, written, toFile);
            if (PHYSFS_eof(fromFile))
              break;
          }
          fclose(toFile);
        }
        PHYSFS_close(fromFile);
      }
    }
    files++;
  }
  PHYSFS_freeList(filesOrig);
}

static void android_vthrow(lua_State* L, const char* format, ...) {
  #define MAX_ERROR_LENGTH 1024
  char lovrErrorMessage[MAX_ERROR_LENGTH];
  va_list args;
  va_start(args, format);
  vsnprintf(lovrErrorMessage, MAX_ERROR_LENGTH, format, args);
  va_end(args);
  __android_log_print(ANDROID_LOG_FATAL, "LOVR", "Error: %s\n", lovrErrorMessage);
  assert(0);
}

static int luax_preloadmodule(lua_State* L, const char* key, lua_CFunction f) {
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_pushcfunction(L, f);
  lua_setfield(L, -2, key);
  lua_pop(L, 2);
  return 0;
}

void bridgeLovrInit(BridgeLovrInitData *initData) {
  __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "\n INSIDE LOVR\n");

  // Save writable data directory for LovrFilesystemInit later
  {
    std::string writablePath = std::string(initData->writablePath) + "/data";
    bridgeLovrWritablePath = strdup(writablePath.c_str());
    mkdir(bridgeLovrWritablePath, 0777);
  }

  // This is a bit fancy. We want to run files off disk instead of out of the zip file.
  // This is for two reasons: Because PhysFS won't let us mount "within" a zip;
  // and because if we run the files out of a temp data directory, we can overwrite them
  // with "adb push" to debug.
  // As a TODO, when PHYSFS_mountSubdir lands, this path should change to only run in debug mode.
  std::string programPath = std::string(initData->writablePath) + "/program";
  {
    // We will store the last apk change time in this "lastprogram" file.
    // We will copy all the files out of the zip into the temp dir, but ONLY if they've changed.
    std::string timePath = std::string(initData->writablePath) + "/lastprogram.dat";

    // When did APK last change?
    struct stat apkstat;
    int statfail = stat(initData->apkPath, &apkstat);
    if (statfail) {
      __android_log_print(ANDROID_LOG_ERROR, "LOVR", "CAN'T FIND APK [%s]\n", initData->apkPath);
      assert(0);
    }

    // When did we last do a file copy?
    timespec previoussec;
    FILE *timeFile = fopen(timePath.c_str(), "r");
    bool copyFiles = !timeFile; // If no lastprogram.dat, we've never copied
    if (timeFile) {
      fread(&previoussec.tv_sec, sizeof(previoussec.tv_sec), 1, timeFile);
      fread(&previoussec.tv_nsec, sizeof(previoussec.tv_nsec), 1, timeFile);
      fclose(timeFile);

      copyFiles = apkstat.st_mtim.tv_sec != previoussec.tv_sec || // If timestamp differs, apk changed
               apkstat.st_mtim.tv_nsec != previoussec.tv_nsec;
    }

    if (copyFiles) {
      __android_log_print(ANDROID_LOG_ERROR, "LOVR", "APK CHANGED [%s] WILL UNPACK\n", initData->apkPath);

      // PhysFS hasn't been inited, so we can temporarily use it as an unzip utility if we deinit afterward
      PHYSFS_init("lovr");
      int success = PHYSFS_mount(initData->apkPath, NULL, 1);
      if (!success) {
        __android_log_print(ANDROID_LOG_ERROR, "LOVR", "FAILED TO MOUNT APK [%s]\n", initData->apkPath);
        assert(0);
      } else {
        physCopyFiles(programPath, "/assets");
      }
      PHYSFS_deinit();

      // Save timestamp in a new lastprogram.dat file
      timeFile = fopen(timePath.c_str(), "w");
      fwrite(&apkstat.st_mtim.tv_sec, sizeof(apkstat.st_mtim.tv_sec), 1, timeFile);
      fwrite(&apkstat.st_mtim.tv_nsec, sizeof(apkstat.st_mtim.tv_nsec), 1, timeFile);
      fclose(timeFile);
    }
  }

  // Unpack init data
  bridgeLovrMobileData.displayDimensions = initData->suggestedEyeTexture;
  bridgeLovrMobileData.updateData.displayTime = initData->zeroDisplayTime;

  // Ready to actually go now.
  // Copypaste the init sequence from lovrRun:
  // Load libraries
  L = luaL_newstate(); // FIXME: Just call main?
  luaL_openlibs(L);
  __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "\n OPENED LIB\n");

  lovrSetErrorCallback((lovrErrorHandler) android_vthrow, L);

  // Install custom print
  static const struct luaL_Reg printHack [] = {
    {"print", lovr_luaB_print_override},
    {NULL, NULL} /* end of array */
  };
  lua_getglobal(L, "_G");
  luaL_register(L, NULL, printHack); // "for Lua versions < 5.2"
  //luaL_setfuncs(L, printlib, 0);  // "for Lua versions 5.2 or greater"
  lua_pop(L, 1);

  glfwSetTime(0);

  // Set "arg" global
  {
    const char *argv[] = {"lovr", programPath.c_str()};
    int argc = 2;

    lua_newtable(L);
    lua_pushstring(L, "lovr");
    lua_rawseti(L, -2, -1);
    for (int i = 0; i < argc; i++) {
      lua_pushstring(L, argv[i]);
      lua_rawseti(L, -2, i == 0 ? -2 : i);
    }
    lua_setglobal(L, "arg");
  }

  // Register loaders for internal packages (since dynamic load does not seem to work on Android)
  luax_preloadmodule(L, "lovr", luaopen_lovr);
  luax_preloadmodule(L, "lovr.audio", luaopen_lovr_audio);
  luax_preloadmodule(L, "lovr.data", luaopen_lovr_data);
  luax_preloadmodule(L, "lovr.event", luaopen_lovr_event);
  luax_preloadmodule(L, "lovr.filesystem", luaopen_lovr_filesystem);
  luax_preloadmodule(L, "lovr.graphics", luaopen_lovr_graphics);
  luax_preloadmodule(L, "lovr.headset", luaopen_lovr_headset);
  luax_preloadmodule(L, "lovr.math", luaopen_lovr_math);
  luax_preloadmodule(L, "lovr.physics", luaopen_lovr_physics);
  luax_preloadmodule(L, "lovr.thread", luaopen_lovr_thread);
  luax_preloadmodule(L, "lovr.timer", luaopen_lovr_timer);
  luax_preloadmodule(L, "cjson", luaopen_cjson);
  luax_preloadmodule(L, "enet", luaopen_enet);

  // Run init

  lua_pushcfunction(L, luax_getstack);
  if (luaL_loadbuffer(L, (const char*) boot_lua, boot_lua_len, "boot.lua") || lua_pcall(L, 0, 1, -2)) {
    __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "\n LUA STARTUP FAILED: %s\n", lua_tostring(L, -1));
    lua_close(L);
    assert(0);
  }

  coroutineStartFunctionRef = luaL_ref(L, LUA_REGISTRYINDEX); // Value returned by boot.lua
  Lcoroutine = lua_newthread(L); // Leave L clear to be used by the draw function
  coroutineRef = luaL_ref(L, LUA_REGISTRYINDEX); // Hold on to the Lua-side coroutine object so it isn't GC'd

  __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "\n BRIDGE INIT COMPLETE top %d\n", (int)lua_gettop(L));
}

void bridgeLovrUpdate(BridgeLovrUpdateData *updateData) {
  // Unpack update data
  bridgeLovrMobileData.updateData = *updateData;

  // Go
  if (coroutineStartFunctionRef != LUA_NOREF) {
    lua_rawgeti(Lcoroutine, LUA_REGISTRYINDEX, coroutineStartFunctionRef);
    luaL_unref (Lcoroutine, LUA_REGISTRYINDEX, coroutineStartFunctionRef);
    coroutineStartFunctionRef = LUA_NOREF; // No longer needed
  }
  if (lua_resume(Lcoroutine, 0) != LUA_YIELD) {
    __android_log_print(ANDROID_LOG_DEBUG, "LOVR", "\n LUA QUIT\n");
    assert(0);
  }
}

void bridgeLovrDraw(BridgeLovrDrawData *drawData) {
  int eye = drawData->eye;
  lovrOculusMobileDraw(drawData->framebuffer, bridgeLovrMobileData.displayDimensions.width, bridgeLovrMobileData.displayDimensions.height,
    bridgeLovrMobileData.updateData.eyeViewMatrix[eye], bridgeLovrMobileData.updateData.projectionMatrix[eye]); // Is this indexing safe?
}

}