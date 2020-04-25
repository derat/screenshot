# screenshot

Small X11 app for taking screenshots of part or all of the screen

## Overview

This repository contains a very-slightly-modified copy of `screenshot.cc`, a
file written in 2009 for use with [chromeos-wm], Chrome OS's original X11-based
window manager.

It is executed from the command line with the name of a PNG file to write. If
the `-region` flag is passed, the program waits for the user to drag a rectangle
using the left mouse button and captures only that region of the screen.
Otherwise, it captures the window specified by the `-window` flag. If no flags
are supplied, the root window (i.e. the entire desktop) is captured.

[chromeos-wm]: https://chromium.googlesource.com/chromiumos/platform/window_manager/

## Usage

```sh
% ./screenshot --help                                                                                                       [~/code/screenshot]
screenshot: Usage: screenshot [FLAGS] FILENAME.png

Saves the contents of the entire screen or of a window to a file.
...
  Flags from screenshot.cc:
    -region (Use the mouse to select a region of the screen to capture)
      type: bool default: false
    -window (Window to capture, as a hexadecimal X ID (if empty, the root
      window is captured)) type: string default: ""
```

## Installation

To compile the program on a Debian-based system, run the following command to
ensure that dependencies are installed:

```sh
sudo apt-get install \
  g++ make pkg-config \
  libx11-dev libcairo2-dev \
  libgflags-dev libgoogle-glog-dev
```

Then compile the program by running `make`.

Copy the resulting `screenshot` binary to a directory listed in `$PATH`, e.g.
`/usr/local/bin`.
