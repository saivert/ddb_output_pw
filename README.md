# PipeWire output plugin for DeaDBeeF

This is a draft for a PipeWire plugin for DeaDBeeF Music Player

Build using meson:

    $ meson setup builddir
    $ meson compile -C builddir

Then install:

    $ cp builddir/ddb_out_pw.so ~/.local/lib64/deadbeef

Remember to have the Deadbeef development package installed or `deadbeef.h` available in standard search paths (`/usr/include/deadbeef` or `/usr/local/include/deadbeef`).

You can instruct meson to search for include files like so:

    $ C_INCLUDE_PATH=/opt meson setup

This assumes the header file is in `/opt/deadbeef` directory.


New plugin settings UI:

![Screenshot](../assets/plugin-settings-newui.png?raw=true)


Older plugin settings UI:

![Screenshot](../assets/pipewire-options.png?raw=true)

