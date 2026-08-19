Version 2026.08.19
------------------
* New features
  * The BMX menu can now be controlled with a mouse after enabling it 
    (Mouse->Menu control). Usage: Left click: opens menu, switches parameters
    between "on" or "off". Holding left mouse button and dragging left/right or
    up/down on a numeric paramter changes the value fine-grained. Using the
    scroll wheel on a numeric parameter changes it in coarse steps. Righ click:
    back. Moving the mouse pointer to the top or button of the menu scrolls it
    up/down. Sensitivit is currently not taken into account during menu 
    navigation. Drag speed can be configured as well.

* Bugfixes
  * A timing fix from 28.7.2026 that fixed audio problems caused new timing 
    issus on x64sc (e.g. demo freeze). Reverted the fix and implemented a new
    one which decouples machine and audio timing.
  * Shift + _ caused wrong displayed characters. The "safe shifted symbols" fix
    has been enhanced to include all virtual modifier keys.

Version 2026.08.17
------------------
* New feature
  * Pi4/Pi400 now uses the full multicore path by default: Circle,
    NetworkService and USB run on Core 0, VICE on Core 1, and the SID worker
    on Core 2 for supported dual-SID configurations.
  * Pi4 HDMI modesetting with RGB565 framebuffer and hardware scaling, 
    providing HDMI resolution switching. This is basically now the same as on
    the Pi5.
  * Pi4 builds are now 64-bit.
  * CRT Effects for both, Pi4 and Pi5.
  * BMX Menu improvements: Dialog scroll indicators, 'DisplayInfo' will not
    overlap with the menu setting anymore.
  * Added five 'Quick Access' slots at the top of the BMX menu for fast access
    to your most used options. Go to the option you would like to add there and
    press SPACE, choose the slot and press ENTER.
  * Pending system changes are now collected and displayed when you press ESC
    or "Apply & Reboot". The latter is only enabled when there are pending
    system changes.

* Bugfixes
  * WiFi stability fixes.
  * Dialog text sometimes extented beyond the dialog box.
  * Shift/Deshift handling was not taken into account.
  * Rendering of PETSCII directories didn't take quote mode into account which
    led to wrongly displayed symbols.

Version 2026.08.14
------------------
* New features
  * Redesigned Circle's network as a Core-0 service.
  * Improved menu navigation: Arrow right: open menu, arrow left: close menu.
    Enter: displays text files. If you return from a folder the cursor is not
    placed on top, but on the position of the folder where you came from.
  * Added a non-developer REST interface. Disabled per default (still work in
    progress).
  * Added a keyboard mapping editor to edit the cusom layouts and/or inspect
    the other available mappings (positional etc.).
  * Replaced the BMX menu font with unscii 2.1. The content of disk images
    is rendered using the native maschine font.
  * The BMX menu now appears in exactly the same size on all maschines. The
    size and the gap between two lines can be configured in Prefs->Menu.
  * Added original Vice symbolic and position keymaps back to BMX. The current
    and default keymap is called "PI/PC (BMX)".

* Bugfixes
  * Backported selected post-3.10 VICE fixes for custom C128 C64 KERNALs, 
    VIA timers and 1541 ports, virtual-drive file creation, VIC-II collision 
    interrupts, and SCPU64 REU DMA.
  * Network performance and stability fixes.
  * Fix for RS232 timing issues.
  * Recover halted xHCI endpoints after USB errors (fix for some Keyboard/Mouse
    combos).
  * 8BitDo V1 dongles were not working.
  * Pressing two keys on the host to emulate one key in the emulated maschine
    can occassionally result in wrong characters (e.g. pressing left shift + +
    many time will result in screen code 64 instead of the expected asterix).
    The fix can be turned on/off in the Keyboard menu.
  * The online updater could not replace files when the filename is different.
    This is will be needed for the next update.

Version 2026.08.06
------------------
* New features
  * Added a Raspberry Pi 4/5 overclocking menu. If something goes wrong, you 
    have to manually edit config.txt! USE AT YOUR OWN RISK!
  * Added a developer mode to BMX which can be enabled/disabled in the system
    menu. When enabled a REST interface and a web ui is available to support 
    development and diagnostics (still work in progress). Nothing will ever
    leave your network!
  * Generic USB HID-Interfaces are now routed to the mouse, gamepad, or 
    touchscreen driver based on their HID Application Collections. 
  * Added a selectable VICE mouse type. Micromys is the default.
  * Added a Mouse monitor for movement, supported buttons, and wheel input
    (depending on the selected mouse type).
  * Added a Keyboard monitor showing raw HID usages, exact `.vkm` tokens,
    modifiers, held keys, the active mapping entry, and its emulated key.
  * Added per-machine `user_pos.vkm` and `user_pos_de.vkm` files that can be 
    modified by users, These files are not overwritten during an online update.
  * Positional DE and Positional US keyboard mappings are now called 
    "Positional". The layout DE or US can be selected with the "USB Keyboard
    Layout" setting.
  * Palettes are now loaded from sdcard (SYS:/palettes, USER:/palettes) and
    marked with [S]ys, [U]ser or [B]uiltin in the BMX menu. The one and only
    builtin palette is "VICE".
  * The 'Configure custom GPIO' menu setting shows the mapping of config options
    #1 - #4. Only config #5 can be edited.
  * Added a GPIO monitor showing the status of the configurable GPIO pins.
  * GPIO outputs can be enabled in the GPIO menu. Manual editing of 
    cmdline.txt is not necessary.

* Bugfixes
  * The host keyboard was interpreted as US during entries (e.g filenames). Now
    it repects the keyboard mapping (in the sense that z and y are swapped).
  * Fixed erratic 1351 mouse movement in GEOS 128 by backporting the VICE
    r46014 POT glitch-emulation correction.
  * Saved keyboard mappings were not correctly restored after a reboot. It
    always started with the Positional DE mapping.
  * Fixed a false "Custom keymap unavailable" error when cycling from
    PETSCIIBOARD to Custom.
  * Multi-report USB HID mouse devices were not handled correctly. Reports are
    now decoded according to their HID report descriptors.
  * Some shifted keys occassionally resulted in a wrong displayed character
    due to an old Vice bug. (eg Left Shift + + displayed occassionally screen
    code 64, instead of asterix). Because I do not know if the fix has any
    side effects, you can turn it on/off in the keyboard menu (safe shifted
    symbols).

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
  * Added support for a user selectable phonebook for BBS connections
    which can be dialed using the "ATDT" command within a terminal (e.g.
    `ATDT0`, `ATDTMYBBS`, or `ATDT515123456`). The format of the .pb file is:
    `shortcut=addr`,
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
