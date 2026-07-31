Version 2026.07.31
------------------
* New features
  * Added a Mouse menu below 'Keyboard'. It shows the detected USB mouse and
    provides a sensitivity setting with a live pointer preview.
  * C64, C64SC, SCPU64 and C128 can load and save raw .REU images from the RAM 
    Expansion menu. Optional automatic write-back is off by default.
  * Image navigation: When you browse disk/tape images you can autostart a 
    program by pressing return. With arrow left/right you can browse in and out 
    of directories.

* Bugfixes
  * Important Display initialization were missing due to the removal of
    assertions in the release build.
  * Pi4B: Ethernet was not working due to a wrong configuration.
  * Pi 4/400: Obtaining an IP address via DHCP could be delayed because an 
    already established PHY link was not detected until a later polling cycle.
  * Multiple registered keyboards were not handled correctly. There was only one
    internal state for all keyboards so that key presses and releases were not   
    interpreted correctly.
  * Provided by user aminch: Fix for 8BitDo Numpad 8 mapping.
  * install_sd.sh did not write the kernel image with the correct extension.
    
Version 2026.07.30
------------------
* New features
  * Support for 8BitDo Ultimate 2 (2.4GHz, Bluetooth, Wired)
  * Provided by aminch: Support for 8BitDo Ultimate C.
  * Productstrings of detected USB devices are displayed for Keyboards,
    Gamepads and Sound devices in the respective menu.
  * PI5: SID 2 is offloaded to a free CPU core.
  * Reworked build system that supports much faster incremental builds.
  * IEC listing of files had a complexity of O(n^2). Now it is O(n).

* Bugfixes
  * Sound devices were switchted after the BMX menu was closed. Now it is 
    switched even if the menu is open, so that the current active device can be 
    correctly displayed.
  * The updater did not allow new directories on the sdcard.
  * The usb sound device was not correctly displayed under all circumstances.
  * ROM selection should not freeze on invalid drive ROMs or overlong paths.

Version 2026.07.28
------------------
* New features
  * Restructured Sound menu.
  * SID filter passband, gain and bias can be changed within the BMX menu and 
    live via a SID Filter OSD.
  * Datasette sound added to the tape menu.
  * Audio leak setting added to the sound menu.
  * Implemented IEC filesystem (could also be a considered a bugfix because it
    wasn't properly ported yet).
  * Circle-known USB gamepads now get an automatic default mapping while
    existing custom mappings remain unchanged.
  * Provided by aminch: CUSBKeyboard8BitDoDevice

* Bugfixes
  * Fix provided by aminch: Moves with a USB Mouse (1351) were not detected due 
    to changes between Vice 3.3 and Vice 3.10.
  * Circle did not respect configured "total_mem".
  * Circle had problems with composite audio devices (an empty function slot was 
    interpreted as end and circle used interface 0 as audio device, although it
    is 1 in this case)
  * Circle used fixed 44100 khz for sound devices. Now it uses what the device 
    offers (44100 preferred, then 48000, else whatever the device returns).
  * Vice was producing frames with 50.124542 HZ. BMX with 50 HZ. This cause 
    audio "bumps". Solution: circle_cycvles_per_sec() (was not correctly ported 
    from BMC64).
  * "Autostart Warp" Status was not displayed in the status bar.
  * 8bitdo v2 was still not 100% stable wenn reconnecting an xbox controller 
    multiple times. The present version behaves basically the same as Linux 
    after sniffing USB traffic on x86 Linux, Raspbian.
  * It wasn't possible to create two different sid models.
  * Loading a snapshot resulted in an emulator freeze.

* Changes
  * All default values in "Color adjustments..." are set to 1000.
  * The 8bitdo code is restructured based on feedback from aminch.

Version 2026.07.24
------------------
* New features
  * Added SuperCPU x64 (experimental).
  * Added x64sc (experimental).
  * Added a diagnostics screen.
  * Sound output priority selectable ("HDMI, USB" or "USB, HDMI").
  * Menu option to reboot and power off the Raspberry Pi.
  * Autostart and 'Attach disk' remember the last used directory.
  * Configurable default drive for utils.d64. Utils.d64 can be changed by the 
    user to something else.
  * Redesigned machine menu.
  * New machines.ini and optimized internal handling (machines.ini is not
    compatible with machines.txt!)
  * Attaching tapes, disks and carts can be mapped to a hotkey.
  * Disk/Tape content viewer (arrow right: view / arrow left: return)
  * USB Plug&Play for Gamepads, Keyboards and Mice
  * Support for online Updates. Uses the same zip file from github that a user 
    would use to manually install BMX. Nothing happens without users knowledge. 
    No popus, no automatic update checks. Everything must be initiated by the 
    user.
  * Improved file handling for big images (Faster, less memory usage): 
    Big files are not completely load into memory anymore, but directly 
    read/written via FatFS.

* Bugfixes
  * Fixed a Circle bug that broke USB Sound.
  * PI4: Activating the status bar caused the bottom part of the screen to 
    flicker while the BMX menu is open. Workaround: The status bar is not 
    visible while the BMX menu is open.
  * Rendering of the visible layers (vic, vdc, menu, status) was not 
    synchronized.
  * Scaling interpolation wrongly showed "Use , and . for -/+1 increment".
  * An activated drive 9 can cause software incompatibilities.
  * File/directory listing was slow due to unneccessary calls to stat() for each 
    entry (FatFs entry attributes are now reused)
  * 8bitDo V2 Dongle (2dc8:3106) was not connectet to the XBox-360 driver.
  * 8bitDo V1&V2 were not correctly initialized.
  * 8bitDo V2 needs USB Plug&Play to function properly.
  * Fixed memory leaks in new_io
  * Fixed a memory leak in PI5KMS framebuffer, which can cause crashes.


Version 2026.07.03
------------------
Initial release

* Core features:
  * Based on VICE 3.10 (C64, C128, VIC20, Plus/4 and PET machines are supported)
  * Circle v20, Step 51 to support the Raspberry Pi4/5 family
  * Pi5/Pi500 HDMI modesetting with RGB565 framebuffer and hardware scaling, 
    providing HDMI resolution switching, which is currently not supported in 
    Circle.
  * Integrated network support (Ethernet/Wi-Fi)
  * Added a popup screen to select available Wi-Fi access points
  * Integrated RS232 over TCP/IP (think connecting two computers with an
    emulated RS232 cable over an internet connection)
  * RS232 supports Userport, UP9600/EZ232 and Swift/Turbo Interfaces
  * Ported tcpser-based Hayes-Modem connected to the RS232 interface within a 
    terminal you can call a BBS like this: "ATDTbmxbbs.de:6510"
  * Added authentic sound options for the Hayes modem
  * Added support for a user selectable phonebook for BBS connections which can 
    be dialed using the "ATDT" command within a terminal (e.g ATDT0, ATDTMYBBS 
    or ATDT515123456). The format of the .pb file is: `shortcut=addr`, 
    e.g. `0=bmxbbs.de:6510`
  * Improvements and bug fixes in the networking stack
  * Support for two partitions on the SD card (a mandatory system/boot
    partition and an optional user partition for disk images, phonebooks etc.)
  * Added support for a C64 utility disk, which is automatically inserted in 
    drive 9 directly after boot. Utility disks for the other machines do not 
    exist yet.
  * The C64 utility disk currently only contains the terminal program "ccgms", 
    modified to default to modem type "Swift/Turbo DE". Enable networking and 
    RS232 in the BMX menu, load ccgms (`LOAD "ccgms",9,1` followed by `RUN`), 
    and you can call a BBS without changing any terminal settings.
  * With the release of the first BMX version a dedicated bbs (bmxbbs.de:6510) 
    was launched. It is the default BBS, which is called when you enter "ATDT" 
    (without any "number") in a terminal program.
