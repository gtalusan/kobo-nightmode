kobo-nightmode
==============

A tiny hack to read white-on-black on Kobo ebook readers. Works only on the eInk series:
+ [Kobo Glo HD](http://kobo.com/koboglohd)
+ [Kobo Aura](http://kobo.com/koboaura)
+ [Kobo Aura HD](http://kobo.com/koboaurahd)
+ [Kobo Glo](http://kobo.com/koboglo)
+ [Kobo Touch](http://kobo.com/kobotouch)
+ [Kobo Mini](http://kobo.com/kobomini)
+ [Kobo Clara HD](https://uk.kobobooks.com/products/kobo-clara-hd)


Still working on 4.10 firmware

Since the update to firmware 2.6+ Kobo has moved to hardware float proccessing, 
requiring a new toolchain and partly breaking binary compatibility with older software. Currently this hack still works on firmwares above 2.6.
If you need help or find a bug, check the [mobileread thread](http://www.mobileread.com/forums/showthread.php?t=212162), or create an issue on github.

Usage
-----
There are three ways to contol nightmode:
+ Long-press the LIGHT / HOME button on your device to toggle
+ Write into the `/tmp/invertScreen` fifo-interface
  + Send `y` to set nightmode to ON
  + Send `n` to set nightmode to OFF
  + Send `t` to toggle
+ Define action 'toggleNightMode' on a (configurable) level of brightness with a (configurable) timeout before the action is taken. By default it is set to:
  + 11% to toggle
  + 12% set nightmode to ON
  + 13% set nightmode to OFF
  + 7 second timeout on brightness level before action is applied

A sample script to toggle is provided in `extra/nightmode.sh`.
As the Kobo Mini has no physical buttons, the fifo-interface is currently the only way to control nightmode when running and thus, 
relies on external tools such as:
+ [KoboLauncher](http://www.mobileread.com/forums/showthread.php?t=201632), maintained by sergeyvl12
+ [Kobo Tweaks](http://www.mobileread.com/forums/showthread.php?t=206180), maintained by ah- (currently not available for 2.6+)

As there is no power button on Glo HD need to adjust setting as described [here] (http://www.mobileread.com/forums/showpost.php?p=3155975&postcount=512) so that
a particular brightness level toggles on the night mode.

Configuration
-------------
The configuration file called `nightmode.ini` is located in your `.kobo` folder:
```ini
# config file for kobo-nightmode

[state]
invertActive = no             # yes / no
retainStateOverRestart = yes  # yes / no
[control]
longPressDurationMS = 800     		# time in milliseconds to toggle (1000 = 1 second)
lightButtonAction = toggleNightMode	# actions associated to long press of light button:
					# - toggleNightMode : toggle the nightmode (default)
					# - launchCommand : launch the command specified by variable "launchButtonCommand"
					# - both : toggle nightmode and launch the command
lightButtonCommand = /mnt/onboard/.kobo/testScript.sh
[nightmode]
refreshScreenPages = 0        # force refresh every X pages
[brightness]
1percentPatch = yes # yes / no : Nickel set brightness to 2% even when on UI it is set to 1%
                    # this patch will force brightenss to 1% when:
                    # - it was set to 1%, then it was set to 0% (for instance: stand-by) then changed again to "what Nickel says is 2%"
                    # - it was set to 3%, then it was set to 1% then changed again to "what Nickel says is 2%"
timeout = 7         # time in seconds, after brightness is set (and maintained) to a given level, to toggle the action set to that level
## syntax for associating the trigger of an action when brightness is set to X%:
## X=<command/script>
## special actions:
## - toggleNightMode: toggle Nightmode on / off
## - enableNightMode: force enabling Nightmode
## - disableNightMode: force disabling Nightmode
3 = toggleNightMode # when brightness is set to 3% : toggle Nightmode
# some examples of invoking any script when brightness is set to X% for at least TIMEOUT seconds
#4 = touch /mnt/onboard/hello.world
#5 = touch /mnt/onboard/hello.world.async &
```
`invertActiveOnStartup` determines whether nightmode is active after booting. 
`retainStateOverRestart` determines whether the state should be kept over a restart.
`refreshScreenPages` sets the number of pages between screen refreshs when nightmode is active.
If you want to disable this feature, set it to 0.

Installation
------------
In the installer/uninstaller directories, two `KoboRoot.tgz` files are provided for automatic installation and removal.
Simply copy one of them into the .kobo folder on your device, safely remove, and wait for the reboot to finish.

How it works
------------
This hack works by intercepting and modifying screen-update (and set brightness level) requests on-the-fly from the main reader application. 
This is accomplished by interposing the `ioctl()` function using LD_PRELOAD.
Therefore `/etc/init.d/rcS` has to be modified to start `nickel`, the main app with the `screenInv.so` dynamic library.
For the Kobo Aura, the inverting has to be done in software due to a kernel bug. By interposing `mmap()`, memory accesses to framebuffer can be redirected into a virtual buffer, from which the inverted data is pushed to the screen.

FAQ
----
+ I have updated my firmware and it stopped working!
  + Simply install the mod again, if it still doesn't work post in the thread.
+ Will this drain my battery?
  + The effect on battery life should be negligible
+ I can see more ghosting when the mod is activated!
  + This is normal, as eInk screens are optimized for black-on-white mode. Change the refresh rate if it annoys you.

Compile for yourself
--------------------
All you need can be found in Kobo's [Kobo-Reader](https://github.com/kobolabs/Kobo-Reader) repository:
+ An linux tarball for the imx507 platform
+ The gcc-linaro-arm-linux-gnueabihf toolchain to compile

Credits
-------
+ [KevinShort](http://www.mobileread.com/forums/member.php?u=154832), for the idea and a first proof of concept
+ [GitHub contributers](https://github.com/dbeinder/kobo-nightmode/graphs/contributors)
+ Nicolas Devillard and others, for the [iniParser](http://github.com/ndevilla/iniparser) library

