#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/types.h>

#include <linux/types.h>
#include <linux/fb.h>
#include "mxcfb.h"
#include <linux/rtc.h>

//alternative headers without function definition
#include <asm-generic/ioctl.h>
#include <asm-generic/mman-common.h>

#include "iniParser/iniparser.h"
#include "screenInv.h"

#define EVIOCGRAB		_IOW('E', 0x90, int)

#define RTC_WAKEUP_FLAG _IOR('p', 0x13, unsigned long)

//interposed funcs
int ioctl(int filp, unsigned long cmd, unsigned long arg);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int execv(const char *filename, char *const argv[]);
FILE *fopen(const char *path, const char *mode);
int open(const char *pathname, int flags,...);

//static int mxc_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
// nel file: file:///home/sergio/Dev/kobo/libs/linux-2.6.35.3/drivers/rtc/rtc-mxc_v2.c

//original funcs
static int (*ioctl_orig)(int filp, unsigned long cmd, unsigned long arg) = NULL;
static void* (*mmap_orig)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
static int (*execv_orig)(const char *filename, char *const argv[]) = NULL;
//static int (*execve_orig)(const char *filename, char *const argv[], char *const envp[]) = NULL;
static FILE* (*fopen_orig)(const char *path, const char *mode) = NULL;
static int (*open_orig)(const char *pathname, int flags) = NULL;

static void initialize() __attribute__ ( ( constructor ) );
static void cleanup() __attribute__ ( ( destructor ) );

static pthread_t cmdReaderThread = 0, buttonReaderThread;
static struct mxcfb_update_data fullUpdRegion, workaroundRegion;
static struct mxcfb_update_data_v2 fullUpdRegion_v2, workaroundRegion_v2;
static int fb0fd = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint16_t *fbMemory = NULL;
static uint16_t *virtualFB = NULL;
static time_t configLastChange = 0;

static dictionary* configIni = NULL;
static bool useHWInvert = true;
static bool useV2API = false;
static bool inversionActive = false;
static bool retainState = false;
static int longPressTimeout = 800;
static int thresholdScreenArea = 0;
static int nightRefresh = 3;
static int nightRefreshCnt = 0;
static char *brightnessActions[101];
static int brightnessTimeout = 5;
static bool brightness1patch = false;
static bool lightButtonToggleNightMode = true;
static bool lightButtonLaunchCommand = false;
static char *lightButtonCommand = NULL;
static int autoSwitchOffTimeoutMinutes = 0;
static bool debug = true;

static long flCounter = 0;
static unsigned long flCurrent = 0;
static unsigned long flPrevious = 0;
struct timespec timeLast0;

static time_t last_rtc_time;

static void forceUpdate() {
    int ret = -1;

    if (!useV2API) {
        if (useHWInvert) {
            fullUpdRegion.flags = inversionActive ? EPDC_FLAG_ENABLE_INVERSION : 0;
            ret = ioctl_orig(fb0fd, MXCFB_SEND_UPDATE, (unsigned long int) &fullUpdRegion);
        } else {
            fullUpdRegion.flags = 0;
            ret = ioctl(fb0fd, MXCFB_SEND_UPDATE, (unsigned long int) &fullUpdRegion);
        }
    } else {
        if (useHWInvert) {
            fullUpdRegion_v2.flags = inversionActive ? EPDC_FLAG_ENABLE_INVERSION : 0;
            ret = ioctl_orig(fb0fd, MXCFB_SEND_UPDATE_V2, (unsigned long int) &fullUpdRegion_v2);
        } else {
            fullUpdRegion_v2.flags = 0;
            ret = ioctl(fb0fd, MXCFB_SEND_UPDATE_V2, (unsigned long int) &fullUpdRegion_v2);
        }
    }

#ifdef SI_DEBUG
    if (ret < 0) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Full redraw failed! Error %d: %s", ret, strerror ( errno ));
        }
        system("dmesg | grep mxc_epdc_fb: > /mnt/onboard/.kobo/screenInvertLogFB");
    }
#else
    ret++; //avoid compiler warning ;)
#endif
}

static void readConfigFile( bool readState) {
    configIni = iniparser_load( SI_CONFIG_FILE);
    if (configIni != NULL) {
        if (readState)
            inversionActive = iniparser_getboolean(configIni, "state:invertActive", 0);

        debug = iniparser_getboolean(configIni, "control:debug", true);
        if (!debug) {
            DEBUGPRINT("ScreenInverter: switching debug mode off (config_file directive)");
        }
        retainState = iniparser_getboolean(configIni, "state:retainStateOverRestart", 0);
        longPressTimeout = iniparser_getint(configIni, "control:longPressDurationMS", 800);
        nightRefresh = iniparser_getint(configIni, "nightmode:refreshScreenPages", 3);

        if (iniparser_getboolean(configIni, "nightmode:forceSWInvert", 0)) {
            if (debug) {
                DEBUGPRINT("ScreenInverter: Forcing SW inversion mode!");
            }
            useHWInvert = false;
        }

        char *lighButtonAction = iniparser_getstring(configIni, "control:lightButtonAction", "toggleNightMode");
        if (!strcmp("launchCommand", lighButtonAction)) {
            lightButtonToggleNightMode = false;
            lightButtonLaunchCommand = true;
        } else if (!strcmp("both", lighButtonAction)) {
            lightButtonToggleNightMode = true;
            lightButtonLaunchCommand = true;
        } else {
            //default: toggleNightMode
            lightButtonToggleNightMode = true;
            lightButtonLaunchCommand = false;
        }

        lightButtonCommand = iniparser_getstring(configIni, "control:lightButtonCommand", '\0');

        char brightnessKey[14];
        brightnessTimeout = iniparser_getint(configIni, "brightness:timeout", 5);
        brightness1patch = iniparser_getboolean(configIni, "brightness:1percentPatch", 0);
        for (int i = 0; i < 101; i++) {
            sprintf(brightnessKey, "brightness:%d", i);
            brightnessActions[i] = iniparser_getstring(configIni, brightnessKey, '\0');
            if (brightnessActions[i]) {
                if (debug) {
                    DEBUGPRINT("Action for brightness %d: '%s'", i, brightnessActions[i]);
                }
            }
        }

        autoSwitchOffTimeoutMinutes = iniparser_getint(configIni, "control:switchOffTimeout", 0);

        if (longPressTimeout < 1)
            longPressTimeout = 800;
        if (nightRefresh < 1)
            nightRefresh = 0;

        if (debug) {
            DEBUGPRINT(
                    "ScreenInverter: Read config: invert(%s), retain(%s), longPressTimeout(%d), nightRefresh(%d), lightButtonAction(%s), lightButtonToggleNightMode(%s), lightButtonLaunchCommand(%s), lightButtonCommand(%s), brightness:timeout(%d), brightness:1percentPatch(%s), autoSwitchOffTimeoutMinutes(%d)",
                    inversionActive ? "yes" : "no", retainState ? "yes" : "no", longPressTimeout, nightRefresh, lighButtonAction, lightButtonToggleNightMode ? "yes" : "no",
                    lightButtonLaunchCommand ? "yes" : "no", lightButtonCommand, brightnessTimeout, brightness1patch ? "yes" : "no", autoSwitchOffTimeoutMinutes);
        }
    } else if (debug) {
        DEBUGPRINT("ScreenInverter: Config file invalid or not found, using defaults");
    }
}

static time_t getLastConfigChange() {
    struct stat confStat;
    if (stat( SI_CONFIG_FILE, &confStat)) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Could no stat() config file");
        }
        return 0;
    }

    return confStat.st_ctime;
}

static void setNewState( bool newState) {

    if (debug) {
        DEBUGPRINT("ScreenInverter: setNewState(%s)", newState ? "yes" : "no");
    }

    inversionActive = newState;
    forceUpdate();

    if (getLastConfigChange() != configLastChange)
        readConfigFile( false);

    if (retainState) {
        iniparser_set(configIni, "state:invertActive", inversionActive ? "yes" : "no");
        FILE *configFP = fopen( SI_CONFIG_FILE, "w");
        if (configFP != NULL) {
            fprintf(configFP, "# config file for kobo-nightmode\n\n");
            iniparser_dump_ini(configIni, configFP);
            fclose(configFP);
        }

        configLastChange = getLastConfigChange();
    }
}

static void *buttonReader(void *arg) {
    uint16_t inputBuffer[16];
    int fd = open("/dev/input/event0", O_NONBLOCK);

    int epollFD = epoll_create(1);
    struct epoll_event readEvent;
    readEvent.events = EPOLLIN;
    readEvent.data.fd = fd;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
    int timeOut = -1; //infinity

    while (1) {
        int err = epoll_wait(epollFD, &readEvent, 1, timeOut);

        if (err == 0) { //nothing to read, but timeout
            timeOut = -1;
            if (debug) {
                DEBUGPRINT("ScreenInverter: Light Button action triggered");
            }
            if (lightButtonToggleNightMode) {
                // toggle night mode
                setNewState(!inversionActive);
            }
            if (lightButtonLaunchCommand) {
                // lauch script
                if (debug) {
                    DEBUGPRINT("ScreenInverter: launching script '%s'", lightButtonCommand);
                }
                system(lightButtonCommand);
            }
            continue;
        }

        if (err > 0) { //data available
            int bytesRead = read(fd, &inputBuffer, sizeof(inputBuffer));
            if (bytesRead != 16)
                continue;

            //Buttons:  0x5a -> FrontLight on/off @Glo, 0x66 -> HOME @Touch
            //to see the data for yourself, execute: "hexdump /dev/input/event0"
            if (inputBuffer[6] == 1 && (inputBuffer[5] == 0x5a || inputBuffer[5] == 0x66))
                timeOut = longPressTimeout;
            else
                timeOut = -1;
        }
    }

    return NULL;
}

static void *cmdReader(void *arg) {
    char input;
    int fd = open( SI_CONTROL_PIPE, O_NONBLOCK);

    int epollFD = epoll_create(1);
    struct epoll_event readEvent;
    readEvent.events = EPOLLIN;
    readEvent.data.fd = fd;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);

    while (1) {
        int err = epoll_wait(epollFD, &readEvent, 1, -1);
        if (err > 0) {
            int bytesRead = read(fd, &input, sizeof(input));

            if (bytesRead == 0) { //writing application left the pipe -> reopen
                close(fd);
                fd = open( SI_CONTROL_PIPE, O_NONBLOCK);
                readEvent.data.fd = fd;
                epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
                continue;
            }

            switch (input) {
            case 't': //toggle
                setNewState(!inversionActive);
                if (debug) {
                    DEBUGPRINT("ScreenInverter: Toggled");
                }
                break;
            case 'y': //yes
                setNewState( true);
                if (debug) {
                    DEBUGPRINT("ScreenInverter: Inversion on");
                }
                break;
            case 'n': //no
                setNewState( false);
                if (debug) {
                    DEBUGPRINT("ScreenInverter: Inversion off");
                }
                break;
            case 10: //ignore linefeed
                break;
            default:
                if (debug) {
                    DEBUGPRINT("ScreenInverter: Unknown command!");
                }
                break;
            }
        }
    }

    return NULL;
}

static bool updateVarScreenInfo() {
    if (ioctl_orig(fb0fd, FBIOGET_FSCREENINFO, (long unsigned) &finfo) < 0) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Couldn't get framebuffer infos!");
        }
        return false;
    }

    if (ioctl_orig(fb0fd, FBIOGET_VSCREENINFO, (long unsigned) &vinfo) < 0) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Couldn't get display dimensions!");
        }
        return false;
    }

    if (debug) {
        DEBUGPRINT("ScreenInverter: Got screen resolution: %dx%d @%dBPP, at %d° degrees rotation", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, 90 * vinfo.rotate);
    }
    thresholdScreenArea = ( SI_AREA_THRESHOLD * vinfo.xres * vinfo.yres) / 100;

    if (!useV2API) {
        fullUpdRegion.update_region.width = vinfo.xres;
        fullUpdRegion.update_region.height = vinfo.yres;
    } else {
        fullUpdRegion_v2.update_region.width = vinfo.xres;
        fullUpdRegion_v2.update_region.height = vinfo.yres;
    }

    return true;
}

static void initialize() {
    ioctl_orig = (int (*)(int filp, unsigned long cmd, unsigned long arg)) dlsym( RTLD_NEXT, "ioctl");
    mmap_orig = (void* (*)(void *addr, size_t length, int prot, int flags, int fd, off_t offset)) dlsym( RTLD_NEXT, "mmap");
    execv_orig = (int (*)(const char *filename, char *const argv[])) dlsym(RTLD_NEXT,"execv");
    //execve_orig = (int (*)(const char *filename, char *const argv[], char *const envp[])) dlsym(RTLD_NEXT,"execve");
    fopen_orig = (FILE* (*)(const char *path, const char *mode)) dlsym( RTLD_NEXT, "fopen");
    open_orig = (int (*)(const char *pathname, int flags))dlsym(RTLD_NEXT,"open");


#ifdef SI_DEBUG
    char execPath[32];
    int end = readlink("/proc/self/exe", execPath, 31);
    execPath[end] = 0;

    remove( SI_DEBUG_LOGPATH);

    //write times in dmesg
    if (debug) {
        system("echo Y > /sys/module/printk/parameters/time");
    }
#endif

    DEBUGPRINT("ScreenInverter: Hooked to %s!", execPath);

    unsetenv("LD_PRELOAD");
    DEBUGPRINT("ScreenInverter: Removed LD_PRELOAD!");

    char *platform = getenv("PLATFORM");
    if (platform && (!strcmp(platform, "mx6sll-ntx") || !strcmp(platform, "mx6ull-ntx"))) {
        useV2API = true;
    }

    //read device
    FILE *devReader = NULL;
    char codename[32];
    DEBUGPRINT("ScreenInverter: Reading device type: ");
    if ((devReader = popen("/bin/kobo_config.sh", "r")) < 0) {
        DEBUGPRINT("... failed!");
        return;
    }

    fgets(codename, sizeof(codename) - 1, devReader);

    int lastChar = strlen(codename) - 1;
    if (codename[lastChar] == '\n')
        codename[lastChar] = 0;

    DEBUGPRINT("%s", codename);

    pclose(devReader);

    if ((fb0fd = open("/dev/fb0", O_RDWR)) == -1) {
        DEBUGPRINT("ScreenInverter: Error opening /dev/fb0!");

        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!");
        return;
    }

    //get the screen's resolution
    if (!updateVarScreenInfo()) {
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!");
        return;
    }

    fbMemory = (uint16_t*) mmap_orig(0, finfo.smem_len, PROT_READ | PROT_WRITE,
    MAP_SHARED, fb0fd, 0);

    if ((int) fbMemory == -1) {
        DEBUGPRINT("ScreenInverter: Failed to map framebuffer device to memory.");

        close(fb0fd);
        return;
    }

    if (!useV2API) {
        fullUpdRegion.update_marker = 999;
        fullUpdRegion.update_region.top = 0;
        fullUpdRegion.update_region.left = 0;
        fullUpdRegion.waveform_mode = WAVEFORM_MODE_AUTO;
        fullUpdRegion.update_mode = UPDATE_MODE_FULL;
        fullUpdRegion.temp = TEMP_USE_AMBIENT;
        fullUpdRegion.flags = 0;

        workaroundRegion.update_marker = 998;
        workaroundRegion.update_region.top = 0;
        workaroundRegion.update_region.left = 0;
        workaroundRegion.update_region.width = 1;
        workaroundRegion.update_region.height = 1; //1px in the top right(!) corner
        workaroundRegion.waveform_mode = WAVEFORM_MODE_AUTO;
        workaroundRegion.update_mode = UPDATE_MODE_PARTIAL;
        workaroundRegion.temp = TEMP_USE_AMBIENT;
        workaroundRegion.flags = 0;
    } else {
        fullUpdRegion_v2.update_marker = 999;
        fullUpdRegion_v2.update_region.top = 0;
        fullUpdRegion_v2.update_region.left = 0;
        fullUpdRegion_v2.waveform_mode = WAVEFORM_MODE_AUTO;
        fullUpdRegion_v2.update_mode = UPDATE_MODE_FULL;
        fullUpdRegion_v2.temp = TEMP_USE_AMBIENT;
        fullUpdRegion_v2.flags = 0;
        fullUpdRegion_v2.dither_mode = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;
        fullUpdRegion_v2.quant_bit = 7;

        workaroundRegion_v2.update_marker = 998;
        workaroundRegion_v2.update_region.top = 0;
        workaroundRegion_v2.update_region.left = 0;
        workaroundRegion_v2.update_region.width = 1;
        workaroundRegion_v2.update_region.height = 1; //1px in the top right(!) corner
        workaroundRegion_v2.waveform_mode = WAVEFORM_MODE_AUTO;
        workaroundRegion_v2.update_mode = UPDATE_MODE_PARTIAL;
        workaroundRegion_v2.temp = TEMP_USE_AMBIENT;
        workaroundRegion_v2.flags = 0;
        workaroundRegion_v2.dither_mode = EPDC_FLAG_USE_DITHERING_PASSTHROUGH;
        workaroundRegion_v2.quant_bit = 7;
    }

    remove( SI_CONTROL_PIPE); //just to be sure
    mkfifo( SI_CONTROL_PIPE, 0600);
    pthread_create(&cmdReaderThread, NULL, cmdReader, NULL);
    pthread_create(&buttonReaderThread, NULL, buttonReader, NULL);

    readConfigFile( true);
    configLastChange = getLastConfigChange();

    //read initial frontlight level from kobo config file: if it's 1%, set variables in order to apply the patch..
    int nickelLightLevel = WEXITSTATUS(system("exit $(cat /mnt/onboard/.kobo/Kobo/Kobo\\ eReader.conf | grep FrontLightLevel | cut -c 17-)"));
    DEBUGPRINT("initial FrontLightLevel (form kobo config file): %d", nickelLightLevel);
    if (nickelLightLevel == 1) {
        //this will enable front light setting to 1% after switching kobo on..
        flPrevious = 1;
        flCurrent = 0;
    }

//    if (debug) {
//        DEBUGPRINT("***RTC_WAKEUP_FLAG %u", RTC_WAKEUP_FLAG);
//        DEBUGPRINT("***RTC_AIE_OFF %u", RTC_AIE_OFF);
//        DEBUGPRINT("***RTC_AIE_ON %u", RTC_AIE_ON);
//        DEBUGPRINT("***RTC_ALM_SET %u", RTC_ALM_SET);
//        DEBUGPRINT("***RTC_ALM_READ %u", RTC_ALM_READ);
//        DEBUGPRINT("***RTC_RD_TIME %u", RTC_RD_TIME);
//        DEBUGPRINT("***RTC_SET_TIME %u", RTC_SET_TIME);
//        DEBUGPRINT("***RTC_EPOCH_SET %u", RTC_EPOCH_SET);
//        DEBUGPRINT("***RTC_WKALM_SET %u", RTC_WKALM_SET);
//        DEBUGPRINT("***RTC_WKALM_RD %u", RTC_WKALM_RD);
//    }

    if (!useHWInvert) {
        virtualFB = (uint16_t *) mmap_orig( NULL, finfo.smem_len,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    }



    //initialize variable for light-change enabling check
    clock_gettime(CLOCK_REALTIME, &timeLast0);
}

static void cleanup() {
    iniparser_freedict(configIni);

    if (cmdReaderThread) {
        pthread_cancel(cmdReaderThread);
        pthread_cancel(buttonReaderThread);
        remove( SI_CONTROL_PIPE);
        close(fb0fd);
        if (debug) {
            DEBUGPRINT("ScreenInverter: Shut down!");
        }
    }
}

static void swCopyRegion(struct mxcfb_rect *region) {
    int pixelPerLine = finfo.line_length / (vinfo.bits_per_pixel / 8);
    int deltaToNextLine = pixelPerLine - region->width;
    int addr = region->top * pixelPerLine + region->left;
    int xStart = region->width;

    if (vinfo.bits_per_pixel == 16) {
        for (int yCnt = region->height; yCnt != 0; yCnt--) {
            for (int xCnt = xStart; xCnt != 0; xCnt--) {
                fbMemory[addr] = virtualFB[addr];
                addr++;
            }

            addr += deltaToNextLine;
        }
    } else if (vinfo.bits_per_pixel == 32) {
        uint32_t *f = (uint32_t *) fbMemory;
        uint32_t *v = (uint32_t *) virtualFB;
        for (int yCnt = region->height; yCnt != 0; yCnt--) {
            for (int xCnt = xStart; xCnt != 0; xCnt--) {
                f[addr] = v[addr];
                addr++;
            }

            addr += deltaToNextLine;
        }
    }
}

static void swInvertCopy(struct mxcfb_rect *region) {
    int pixelPerLine = finfo.line_length / (vinfo.bits_per_pixel / 8);
    int deltaToNextLine = pixelPerLine - region->width;
    int addr = region->top * pixelPerLine + region->left;
    int xStart = region->width;

    if (vinfo.bits_per_pixel == 16) {
        for (int yCnt = region->height; yCnt != 0; yCnt--) {
            for (int xCnt = xStart; xCnt != 0; xCnt--) {
                fbMemory[addr] = 0xffff - virtualFB[addr];
                addr++;
            }

            addr += deltaToNextLine;
        }
    } else if (vinfo.bits_per_pixel == 32) {
        uint32_t *f = (uint32_t *) fbMemory;
        uint32_t *v = (uint32_t *) virtualFB;
        for (int yCnt = region->height; yCnt != 0; yCnt--) {
            for (int xCnt = xStart; xCnt != 0; xCnt--) {
                f[addr] = 0x00ffffff - v[addr];
                addr++;
            }

            addr += deltaToNextLine;
        }
    }
}
/*
 void benchmark(struct mxcfb_rect *region)
 {
 struct timeval stop, start;

 gettimeofday(&start, NULL);
 for(int i = 0; i < 10; i++)
 swCopyRegion(region);
 gettimeofday(&stop, NULL);
 uint64_t diff = ((uint64_t)stop.tv_sec * 1000000 + (uint64_t)stop.tv_usec) - ((uint64_t)start.tv_sec * 1000000 + (uint64_t)start.tv_usec);
 DEBUGPRINT("10x copy took %" PRIu64 "", diff);
 }*/

/* this function is run by the second thread */
void *processFlChange(void *flCounterStart_void_ptr) {
    long flCounterStart = *((long *) flCounterStart_void_ptr);
    free(flCounterStart_void_ptr);
    sleep(brightnessTimeout);

    unsigned long flCurrentProcessed = flCurrent;
    if (flCounter == flCounterStart) {
        flCounter++;
        if (debug) {
            DEBUGPRINT("executing trigger for flCounter:%ld flCurrent:%ld", flCounterStart, flCurrentProcessed);
        }
        if (flCurrentProcessed >= 0 && flCurrentProcessed <= 100 && brightnessActions[flCurrentProcessed]) {
            if (!strcmp(brightnessActions[flCurrentProcessed], "toggleNightMode")) {
                if (debug) {
                    DEBUGPRINT("brightness trigger, toggleNightMode");
                }
                setNewState(!inversionActive);
            } else if (!strcmp(brightnessActions[flCurrentProcessed], "enableNightMode")) {
                if (debug) {
                    DEBUGPRINT("brightness trigger, nightMode enabled");
                }
                setNewState( true);
            } else if (!strcmp(brightnessActions[flCurrentProcessed], "disableNightMode")) {
                if (debug) {
                    DEBUGPRINT("brightness trigger, nightMode disabled");
                }
                setNewState( false);
            } else {
                if (debug) {
                    DEBUGPRINT("brightness trigger, executing command: '%s'", brightnessActions[flCurrentProcessed]);
                }
                system(brightnessActions[flCurrentProcessed]);
            }
        }
    }

    /* the function must return something - NULL will do */
    return NULL;
}

int ioctl(int filp, unsigned long cmd, unsigned long arg) {

	bool show_return_value = false;

    if (cmd == MXCFB_SEND_UPDATE) {

        struct mxcfb_update_data *region = (struct mxcfb_update_data *) arg;

        /*
         DEBUGPRINT ( "ScreenInverter: update: type:%s, flags:0x%x, size:%dx%d (%d%% updated)",
         region->update_mode == UPDATE_MODE_PARTIAL? "partial" : "full", region->flags,
         region->update_region.width, region->update_region.height,
         ( 100*region->update_region.width*region->update_region.height ) /
         ( fullUpdRegion.update_region.width*fullUpdRegion.update_region.height ) );
         */

        if (inversionActive && nightRefresh) {
            if (region->update_region.width * region->update_region.height >= thresholdScreenArea) {
                nightRefreshCnt++;
                if (nightRefreshCnt >= nightRefresh) {
                    region->update_region.top = 0;
                    region->update_region.left = 0;
                    region->update_region.width = fullUpdRegion.update_region.width;
                    region->update_region.height = fullUpdRegion.update_region.height;
                    region->update_mode = UPDATE_MODE_FULL;
                    nightRefreshCnt = 0;
                    if (debug) {
                        DEBUGPRINT("ScreenInverter: nightRefresh: refreshing screen");
                    }
                } else {
                    region->update_mode = UPDATE_MODE_PARTIAL;
                    if (debug) {
                        DEBUGPRINT("ScreenInverter: nightRefresh: no refresh, page %d", nightRefreshCnt);
                    }
                }
            } else {
                if (debug) {
                    DEBUGPRINT("ScreenInverter: nightRefresh: small update, ignoring");
                }
            }
        }

        if (useHWInvert) {
            if (inversionActive) {
                ioctl_orig(filp, MXCFB_SEND_UPDATE, (long unsigned) &workaroundRegion);
                //necessary because there's a bug in the driver (or i'm doing it wrong):
                //  i presume the device goes into some powersaving mode when usb und wifi are not used (great for debugging ^^)
                //  it takes about 10sec after the last touch to enter this mode. after that it is necessary to issue a screenupdate
                //  without inversion flag, otherwise it will ignore the inversion flag and draw normally (positive).
                //  so i just update a 1px region in the top-right corner, this costs no time and the pixel should be behind the bezel anyway.

                region->flags ^= EPDC_FLAG_ENABLE_INVERSION;
            }
        } else {
            if (inversionActive)
                swInvertCopy(&region->update_region);
            else
                swCopyRegion(&region->update_region);
        }
    } else if (cmd == MXCFB_SEND_UPDATE_V2) {
        struct mxcfb_update_data_v2 *region = (struct mxcfb_update_data_v2 *) arg;

        if (inversionActive && nightRefresh) {
            if (region->update_region.width * region->update_region.height >= thresholdScreenArea) {
                nightRefreshCnt++;
                if (nightRefreshCnt >= nightRefresh) {
                    region->update_region.top = 0;
                    region->update_region.left = 0;
                    region->update_region.width = fullUpdRegion_v2.update_region.width;
                    region->update_region.height = fullUpdRegion_v2.update_region.height;
                    region->update_mode = UPDATE_MODE_FULL;
                    nightRefreshCnt = 0;
                    if (debug) {
                        DEBUGPRINT("ScreenInverter: nightRefresh: refreshing screen");
                    }
                } else {
                    region->update_mode = UPDATE_MODE_PARTIAL;
                    if (debug) {
                        DEBUGPRINT("ScreenInverter: nightRefresh: no refresh, page %d", nightRefreshCnt);
                    }
                }
            } else {
                if (debug) {
                    DEBUGPRINT("ScreenInverter: nightRefresh: small update, ignoring");
                }
            }
        }

        if (useHWInvert) {
            if (inversionActive) {
                ioctl_orig(filp, MXCFB_SEND_UPDATE_V2, (long unsigned) &workaroundRegion_v2);
                //necessary because there's a bug in the driver (or i'm doing it wrong):
                //  i presume the device goes into some powersaving mode when usb und wifi are not used (great for debugging ^^)
                //  it takes about 10sec after the last touch to enter this mode. after that it is necessary to issue a screenupdate
                //  without inversion flag, otherwise it will ignore the inversion flag and draw normally (positive).
                //  so i just update a 1px region in the top-right corner, this costs no time and the pixel should be behind the bezel anyway.

                region->flags ^= EPDC_FLAG_ENABLE_INVERSION;
            }
        } else {
            if (inversionActive)
                swInvertCopy(&region->update_region);
            else
                swCopyRegion(&region->update_region);
        }

    } else if (cmd == FBIOPUT_VSCREENINFO) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Screen dimensions changed, updating...");
        }
        int ret = ioctl_orig(filp, cmd, arg); //neccessary, since the kernel makes changes to var & fix infos
        updateVarScreenInfo();
        return ret;
    } else if (cmd == EVIOCGRAB) {
        if (debug) {
            DEBUGPRINT("ScreenInverter: Ignoring nickels request for exclusive access to /dev/input/event0");
        }
        return 0;
    } else if (cmd == 241) {
        if (brightness1patch) {
            //special case: Nickel returns 2 even when on UI the brightness is set to 1%
            // so when it change 1->0->2 --> set to 1
            //                   3->2->2 --> set to 1
            //					>3->2    --> set to 1
            if (arg == 2 && flCurrent == 0 && flPrevious == 1) {
                if (debug) {
                    DEBUGPRINT("forcing brightness to 1 instead of 2 (1 -> 0 -> 2=>1)");
                }
                // turning on light when it was set to 1:
                // first of all set it to 2 (otherwise it will not turn on)
                ioctl_orig(filp, cmd, arg);
                // then set it to 1
                arg = 1;
            } else if (arg == 2 && flCurrent == 2 && flPrevious == 3) {
                if (debug) {
                    DEBUGPRINT("forcing brightness to 1 instead of 2 (3 -> 2 -> 2=>1)");
                }
                // stepping down from 3 to 2 to 1
                arg = 1;
            } else if (arg == 2 && flCurrent > 3) {
                if (debug) {
                    DEBUGPRINT("forcing brightness to 1 instead of 2 (>3 -> 2=>1)");
                }
                // stepping down from >3 to 1
                arg = 1;
            }
        }
        struct timespec now;
        if (arg == 0 || flCurrent == 0) {
            clock_gettime(CLOCK_REALTIME, &timeLast0);
        }
        clock_gettime(CLOCK_REALTIME, &now);
        //a command associated to light change is only processed after 2 seconds than light has been turned on (to prevent that a command is executed after boot/resume/switch light on)
        bool commandEnabled = (1000 * (now.tv_sec - timeLast0.tv_sec) + round(now.tv_nsec / 1000000) - round(timeLast0.tv_nsec / 1000000)) > 2000;
        flPrevious = flCurrent;
        flCurrent = arg;
        if (debug) {
            DEBUGPRINT("ScreenInverter: Command setting frontlight from %ld to %ld (treshold: %f s)", flPrevious, arg, (now.tv_sec - timeLast0.tv_sec) + (round(now.tv_nsec / 1000000) - round(timeLast0.tv_nsec / 1000000))/1000);
        }
        flCounter = (flCounter + 1) % 20000;
        if (arg > 0 && commandEnabled && arg <= 100 && brightnessActions[arg]) {
            pthread_t thread;
            long *flCounterArg = malloc(sizeof(*flCounterArg));
            *flCounterArg = flCounter;
            if (pthread_create(&thread, 0, processFlChange, flCounterArg)) {
                if (debug) {
                    DEBUGPRINT("cannot create thread");
                }
            }
        }
    } else if(autoSwitchOffTimeoutMinutes>0 && cmd==RTC_RD_TIME){
		int rv = ioctl_orig(filp, cmd, arg);
		struct rtc_time *rtc = (struct rtc_time *)arg;
		struct tm tm_rtc;
		memset(&tm_rtc, 0, sizeof tm_rtc);
		tm_rtc.tm_sec = rtc->tm_sec;
		tm_rtc.tm_min = rtc->tm_min;
		tm_rtc.tm_hour = rtc->tm_hour;
		tm_rtc.tm_mday = rtc->tm_mday;
		tm_rtc.tm_mon = rtc->tm_mon;
		tm_rtc.tm_year = rtc->tm_year;
		tm_rtc.tm_isdst = -1;  /* assume the system knows better than the RTC */
		last_rtc_time = mktime(&tm_rtc);
		if(debug){
			int hour = rtc->tm_hour;
			int min = rtc->tm_min;
			int sec = rtc->tm_sec;
			int day = rtc->tm_mday;
			int mon = 1+rtc->tm_mon;
			int year = 1900+rtc->tm_year;
			DEBUGPRINT("ioctl RTC_RD_TIME (utc) %04d/%02d/%02d %02d:%02d:%02d rv: %d",year,mon,day,hour,min,sec,rv);
		}
		return rv;
	} else if(autoSwitchOffTimeoutMinutes>0 && cmd==RTC_WKALM_SET){
		show_return_value = true;
		struct rtc_wkalrm *alrm = (struct rtc_wkalrm *)arg;
		//if(alrm->enabled){
			struct tm tm_rtc;
			memset(&tm_rtc, 0, sizeof tm_rtc);
			tm_rtc.tm_sec = alrm->time.tm_sec;
			tm_rtc.tm_min = alrm->time.tm_min;
			tm_rtc.tm_hour = alrm->time.tm_hour;
			tm_rtc.tm_mday = alrm->time.tm_mday;
			tm_rtc.tm_mon = alrm->time.tm_mon;
			tm_rtc.tm_year = alrm->time.tm_year;
			tm_rtc.tm_isdst = -1;  /* assume the system knows better than the RTC */
			time_t alrm_time = mktime(&tm_rtc);
			double diff_sec = difftime(alrm_time, last_rtc_time);
			if(debug){
				int hour = alrm->time.tm_hour;
				int min = alrm->time.tm_min;
				int sec = alrm->time.tm_sec;
				int day = alrm->time.tm_mday;
				int mon = 1+alrm->time.tm_mon;
				int year = 1900+alrm->time.tm_year;
				unsigned char enabled = alrm->enabled;
				unsigned char pending = alrm->pending;
				DEBUGPRINT("ioctl RTC_WKALM_SET (utc) %04d/%02d/%02d %02d:%02d:%02d enabled:%u pending:%u",year,mon,day,hour,min,sec, enabled, pending);
				DEBUGPRINT("      alarm_time: %ld - last_rtc_time: %ld - diff: %f",alrm_time, last_rtc_time, diff_sec);
			}
			if(diff_sec>540){
				struct rtc_wkalrm myalrm;
				struct tm *tm;
				time_t new_alrm = last_rtc_time + ((autoSwitchOffTimeoutMinutes-1)*60);
				tm = localtime(&new_alrm);
				myalrm.time.tm_sec = tm->tm_sec;
				myalrm.time.tm_min = tm->tm_min;
				myalrm.time.tm_hour = tm->tm_hour;
				myalrm.time.tm_mday = tm->tm_mday;
				myalrm.time.tm_mon = tm->tm_mon;
				myalrm.time.tm_year = tm->tm_year;
				/* wday, yday, and isdst fields are unused by Linux */
				myalrm.time.tm_wday = -1;
				myalrm.time.tm_yday = -1;
				myalrm.time.tm_isdst = -1;
				myalrm.enabled = alrm->enabled;
				myalrm.pending = alrm->pending;
				int rv = ioctl_orig(filp, cmd, &myalrm);
				if(debug){
					int hour = myalrm.time.tm_hour;
					int min = myalrm.time.tm_min;
					int sec = myalrm.time.tm_sec;
					int day = myalrm.time.tm_mday;
					int mon = 1+myalrm.time.tm_mon;
					int year = 1900+myalrm.time.tm_year;
					DEBUGPRINT("changing poweroff interval to: %ld s -> (utc) %04d/%02d/%02d %02d:%02d:%02d rv: %d",new_alrm,year,mon,day,hour,min,sec,rv);
				}
				return rv;
			//}
		}
	}

//    if (debug) {
//        DEBUGPRINT("ioctl function, cmd: %lu. type: %lu, dir: %lu, nr: %lu, size: %lu | arg: %lu", cmd, _IOC_TYPE(cmd), _IOC_DIR(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd), arg);
//    }
    if (debug) {
    	if(cmd==RTC_SET_TIME){
    		show_return_value = true;
    		struct rtc_time *tm = (struct rtc_time *)arg;
			int hour = tm->tm_hour;
			int min = tm->tm_min;
			int sec = tm->tm_sec;
			int day = tm->tm_mday;
			int mon = 1+tm->tm_mon;
			int year = 1900+tm->tm_year;
			DEBUGPRINT("ioctl RTC_SET_TIME (utc) %04d/%02d/%02d %02d:%02d:%02d",year,mon,day,hour,min,sec);
    	} else if (cmd==RTC_WAKEUP_FLAG){
    		int rv = ioctl_orig(filp, cmd, arg);
    		unsigned long *flag = (unsigned long *)arg;
    		DEBUGPRINT("ioctl RTC_WAKEUP_FLAG %lu, rv: %d",*flag, rv);
    		if(*flag==1 && debug){
    			system("dmesg -T > /mnt/onboard/.kobo/dmesg_$(date +%s).log");
    		}
    		return rv;
    	} else if (cmd==RTC_ALM_SET){
    		show_return_value = true;
    		struct rtc_time *tm = (struct rtc_time *)arg;
			int hour = tm->tm_hour;
			int min = tm->tm_min;
			int sec = tm->tm_sec;
			int day = tm->tm_mday;
			int mon = 1+tm->tm_mon;
			int year = 1900+tm->tm_year;
			DEBUGPRINT("ioctl RTC_ALM_SET (utc) %04d/%02d/%02d %02d:%02d:%02d",year,mon,day,hour,min,sec);
    	} else if (cmd==RTC_AIE_ON){
    		show_return_value = true;
    		DEBUGPRINT("ioctl RTC_AIE_ON %lu",arg);
    	}
    }

    int return_value = ioctl_orig(filp, cmd, arg);
    if(debug && show_return_value){
    	DEBUGPRINT("ioctl return value: %d",return_value);
    }
    return return_value;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!useHWInvert && length == finfo.smem_len) {
        char link[32];
        char path[32];
        sprintf(link, "/proc/self/fd/%d", fd);
        int end = readlink(link, path, 31);
        path[end] = 0;
        if (strncmp("/dev/fb0", path, 31) == 0) { //okay, nickel is mmap'ing the framebuffer
            if (debug) {
                DEBUGPRINT("ScreenInverter: mmap'ed the virtual framebuffer!");
            }
            return virtualFB;
        }
    }
    return mmap_orig(addr, length, prot, flags, fd, offset);
}

//int execve(const char *filename, char *const argv[], char *const envp[])
//{
//    if (debug) {
//        DEBUGPRINT("ScreenInverter: execve: %s", filename);
//        for(int i = 0; argv[i] != '\0'; i++){
//        	DEBUGPRINT("  argv[%d] is %s", i, argv[i]);
//        }
//        for(int i = 0; envp[i] != '\0'; i++){
//        	DEBUGPRINT("  envp[%d] is %s", i, envp[i]);
//        }
//    }
//	return execve_orig(filename,argv,envp);
//}

int execv(const char *filename, char *const argv[])
{
//    if (debug) {
//        DEBUGPRINT("ScreenInverter: execv: %s", filename);
//        for(int i = 0; argv[i] != '\0'; i++){
//        	DEBUGPRINT("  argv[%d] is %s", i, argv[i]);
//        }
//    }
    bool found = false;
    for(int i = 0; argv[i] != '\0' && !found; i++){
    	found = i==2 && !strcmp("echo mem > /sys/power/state", argv[i]);
    }
    if (found) {
    	if(debug) {
    		DEBUGPRINT("preparing to suspend (echo mem > /sys/power/state)..");
    	}
    }
	return execv_orig(filename,argv);
}

FILE *fopen(const char *path, const char *mode)
{
//	if(debug) {
//		DEBUGPRINT("fopen: '%s' mode: %s",path, mode);
//	}
    return fopen_orig(path, mode);
}

int open(const char *pathname, int flags, ...)
{
//	if(debug) {
//		DEBUGPRINT("open: '%s' flags: %d",pathname, flags);
//	}
    return open_orig(pathname, flags);
}

