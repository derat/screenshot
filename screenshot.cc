// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <sstream>
#include <sys/time.h>

#include <cairo/cairo.h>
#include <gflags/gflags.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef USE_GLOG
#include <glog/logging.h>
#else
#include "base/logging.h"
#endif

DEFINE_string(window, "",
              "Window to capture, as a hexadecimal X ID "
              "(if empty, the root window is captured)");

DEFINE_bool(region, false,
            "Use the mouse to select a region of the screen to capture");

using std::hex;
using std::istringstream;
using std::max;
using std::min;
using std::numeric_limits;

namespace {

static const char* kUsage =
    "Usage: screenshot [FLAGS] FILENAME.png\n"
    "\n"
    "Saves the contents of the entire screen or of a window to a file.";

// How opaque should the window that we flash onscreen to provide visual
// feedback after the screenshot is taken be (assuming that there's a
// compositing manager running that honors _NET_WM_WINDOW_OPACITY)?
static const double kVisualFeedbackWindowOpacity = 0.25;

// How long should the visual feedback window be displayed?
static const uint64_t kVisualFeedbackWindowDisplayTimeMs = 100;

// Lets the user drag a box to select a region of the screen.
class RegionSelector {
 public:
  explicit RegionSelector(Display* display)
      : display_(display),
        root_(DefaultRootWindow(display_)),
        cursor_(XCreateFontCursor(display_, XC_cross)),
        left_win_(CreateWindow()),
        right_win_(CreateWindow()),
        top_win_(CreateWindow()),
        bottom_win_(CreateWindow()) {
    XGCValues values;
    values.fill_style = FillSolid;
    const unsigned long value_mask = GCForeground | GCBackground | GCFillStyle;

    values.foreground = values.background =
        BlackPixel(display_, DefaultScreen(display_));
    black_gc_ = XCreateGC(display_, root_, value_mask, &values);

    values.foreground = values.background =
        WhitePixel(display_, DefaultScreen(display_));
    white_gc_ = XCreateGC(display_, root_, value_mask, &values);
  }

  ~RegionSelector() {
    XDestroyWindow(display_, left_win_);
    XDestroyWindow(display_, right_win_);
    XDestroyWindow(display_, top_win_);
    XDestroyWindow(display_, bottom_win_);
    XFreeCursor(display_, cursor_);
    XFreeGC(display_, black_gc_);
    XFreeGC(display_, white_gc_);
  }

  // Returns false on failure (e.g. couldn't grab, user aborted, etc.).
  bool SelectRegion(int* x, int* y,
                    unsigned int* width, unsigned int* height) {
    if (!GrabPointer())
      return false;

    // Retry the keyboard grab if it fails -- it may be briefly grabbed by the
    // keyboard shortcut that launched the screenshot program.
    int num_failed_grabs = 0;
    while (!GrabKeyboard()) {
      num_failed_grabs++;
      if (num_failed_grabs >= kMaxKeyboardGrabAttempts) {
        XUngrabPointer(display_, CurrentTime);
        return false;
      }
      usleep(kKeyboardGrabDelayMs * 1000);
    }

    MoveWindowsOffscreen();
    XMapWindow(display_, left_win_);
    XMapWindow(display_, right_win_);
    XMapWindow(display_, top_win_);
    XMapWindow(display_, bottom_win_);

    bool done = false, dragging = false, aborted = false;
    int start_x = 0, start_y = 0, end_x = 0, end_y = 0;
    while (!done && !aborted) {
      XEvent event;
      XNextEvent(display_, &event);
      switch (event.type) {
        case ButtonPress:
          start_x = event.xbutton.x_root;
          start_y = event.xbutton.y_root;
          dragging = true;
          break;
        case ButtonRelease:
          if (dragging) {
            end_x = event.xbutton.x_root;
            end_y = event.xbutton.y_root;
            done = true;
          }
          break;
        case Expose:
          PaintWindow(event.xexpose.window, start_x, start_y, end_x, end_y);
          break;
        case KeyPress:
          if (event.xkey.keycode == XKeysymToKeycode(display_, XK_Escape)) {
            // If we're in a drag, cancel it; otherwise, abort the selection.
            if (dragging) {
              dragging = false;
              MoveWindowsOffscreen();
            } else {
              aborted = true;
            }
          }
          break;
        case MotionNotify:
          if (dragging) {
            end_x = event.xmotion.x_root;
            end_y = event.xmotion.y_root;
            ConfigureWindows(start_x, start_y, end_x, end_y);
          }
          break;
      }
    }

    XUngrabKeyboard(display_, CurrentTime);
    XUngrabPointer(display_, CurrentTime);
    XUnmapWindow(display_, left_win_);
    XUnmapWindow(display_, right_win_);
    XUnmapWindow(display_, top_win_);
    XUnmapWindow(display_, bottom_win_);

    if (aborted)
      return false;

    *x = min(start_x, end_x);
    *y = min(start_y, end_y);
    *width = static_cast<unsigned int>(max(start_x, end_x) - *x);
    *height = static_cast<unsigned int>(max(start_y, end_y) - *y);
    return (width > 0 && height > 0);
  }

 private:
  // Total width of the (black) region border, in pixels.
  static const int kBorder = 2;

  // Width of the inner (white) part of the region border, in pixels.
  static const int kInteriorBorder = 1;

  // Maximum number of times that we'll attempt to grab the keyboard.
  static const int kMaxKeyboardGrabAttempts = 10;

  // Delay before we retry grabbing the keyboard, in milliseconds.
  static const int kKeyboardGrabDelayMs = 100;

  // Create and return an offscreen border window.  Doesn't map it.
  Window CreateWindow() {
    XSetWindowAttributes attr;
    attr.background_pixel = BlackPixel(display_, DefaultScreen(display_));
    attr.override_redirect = True;
    Window win = XCreateWindow(display_,
                               root_,           // parent
                               -1, -1, 1, 1,    // geometry
                               0,               // border_width
                               CopyFromParent,  // depth
                               InputOutput,     // class
                               NULL,            // visual
                               CWBackPixel | CWOverrideRedirect,
                               &attr);
    XSelectInput(display_, win, ExposureMask);
    return win;
  }

  // Configure all of the border windows to frame the current dragged region.
  void ConfigureWindows(int start_x, int start_y, int drag_x, int drag_y) {
    const int left = min(drag_x, start_x);
    const int right = max(drag_x, start_x);
    const int top = min(drag_y, start_y);
    const int bottom = max(drag_y, start_y);

    XMoveResizeWindow(display_, left_win_,
                      left - kBorder, top,
                      kBorder, max(bottom - top, 1));
    XMoveResizeWindow(display_, right_win_,
                      right, top,
                      kBorder, max(bottom - top, 1));
    XMoveResizeWindow(display_, top_win_,
                      left - kBorder, top - kBorder,
                      right - left + 2 * kBorder, kBorder);
    XMoveResizeWindow(display_, bottom_win_,
                      left - kBorder, bottom,
                      right - left + 2 * kBorder, kBorder);
  }

  // Move all of the border windows offscreen.
  void MoveWindowsOffscreen() {
    XMoveResizeWindow(display_, left_win_, -1, -1, 1, 1);
    XMoveResizeWindow(display_, right_win_, -1, -1, 1, 1);
    XMoveResizeWindow(display_, top_win_, -1, -1, 1, 1);
    XMoveResizeWindow(display_, bottom_win_, -1, -1, 1, 1);
  }

  // Repaint a border window.
  void PaintWindow(Window win,
                   int start_x, int start_y,
                   int drag_x, int drag_y) {
    const int width = max(start_x, drag_x) - min(start_x, drag_x);
    const int height = max(start_y, drag_y) - min(start_y, drag_y);

    if (win == left_win_) {
      XFillRectangle(display_, win, black_gc_,
                     0, 0, kBorder - kInteriorBorder, height);
      XFillRectangle(display_, win, white_gc_,
                     kBorder - kInteriorBorder, 0, kInteriorBorder, height);
    } else if (win == right_win_) {
      XFillRectangle(display_, win, black_gc_,
                     kInteriorBorder, 0, kBorder - kInteriorBorder, height);
      XFillRectangle(display_, win, white_gc_,
                     0, 0, kInteriorBorder, height);
    } else if (win == top_win_) {
      XFillRectangle(display_, win, black_gc_,
                     0, 0, width + 2 * kBorder, kBorder - kInteriorBorder);
      XFillRectangle(display_, win, black_gc_,
                     0, kBorder - kInteriorBorder,
                     kBorder - kInteriorBorder, kInteriorBorder);
      XFillRectangle(display_, win, black_gc_,
                     kBorder + width + kInteriorBorder,
                     kBorder - kInteriorBorder,
                     kBorder - kInteriorBorder, kInteriorBorder);
      XFillRectangle(display_, win, white_gc_,
                     kBorder - kInteriorBorder, kBorder - kInteriorBorder,
                     width + 2 * kInteriorBorder, kInteriorBorder);
    } else if (win == bottom_win_) {
      XFillRectangle(display_, win, black_gc_,
                     0, kInteriorBorder,
                     width + 2 * kBorder, kBorder - kInteriorBorder);
      XFillRectangle(display_, win, black_gc_,
                     0, 0, kBorder - kInteriorBorder, kInteriorBorder);
      XFillRectangle(display_, win, black_gc_,
                     kBorder + width + kInteriorBorder, 0,
                     kBorder - kInteriorBorder, kInteriorBorder);
      XFillRectangle(display_, win, white_gc_,
                     kBorder - kInteriorBorder, 0,
                     width + 2 * kInteriorBorder, kInteriorBorder);
    }
  }

  // Grab the pointer, returning true if successful.
  bool GrabPointer() {
    const unsigned int event_mask =
        PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    int grab_result = XGrabPointer(display_,
                                   root_,          // grab_window
                                   False,          // owner_events
                                   event_mask,
                                   GrabModeAsync,  // pointer_mode
                                   GrabModeAsync,  // keyboard_mode
                                   None,           // confine_to
                                   cursor_,
                                   CurrentTime);
    return (grab_result == GrabSuccess);
  }

  // Grab the keyboard, returning true if successful.
  bool GrabKeyboard() {
    int grab_result = XGrabKeyboard(display_,
                                    root_,          // grab_window
                                    False,          // owner_events
                                    GrabModeAsync,  // pointer_mode
                                    GrabModeAsync,  // keyboard_mode
                                    CurrentTime);
    return (grab_result == GrabSuccess);
  }

  Display* display_;
  Window root_;
  Cursor cursor_;
  Window left_win_, right_win_, top_win_, bottom_win_;
  GC black_gc_, white_gc_;
};


// Create and return a window that can be displayed after the screenshot is
// taken to provide visual feedback.  Doesn't map the window.
Window CreateVisualFeedbackWindow(Display* display,
                                  int x, int y,
                                  unsigned int width, unsigned int height) {
  XSetWindowAttributes attr;
  attr.background_pixel = WhitePixel(display, DefaultScreen(display));
  attr.override_redirect = True;
  Window win = XCreateWindow(display,
                             DefaultRootWindow(display),
                             x, y, width, height,
                             0,               // border_width
                             CopyFromParent,  // depth
                             InputOutput,     // class
                             NULL,            // visual
                             CWBackPixel | CWOverrideRedirect,
                             &attr);

  const uint32_t opacity =
      kVisualFeedbackWindowOpacity * numeric_limits<uint32_t>::max();
  XChangeProperty(display,
                  win,
                  XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False),
                  XA_CARDINAL,      // type
                  32,               // format (bits/item)
                  PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&opacity),
                  1);               // num items

  return win;
}

}  // namespace

int main(int argc, char** argv) {
  google::SetUsageMessage(kUsage);
  google::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 2) {
    google::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  Display* display = XOpenDisplay(NULL);
  CHECK(display);

  const char* filename = argv[1];

  Window win = None;
  if (FLAGS_window.empty() || FLAGS_region) {
    win = DefaultRootWindow(display);
  } else {
    istringstream input(FLAGS_window);
    CHECK(!(input >> hex >> win).fail())
        << "Unable to parse \"" << FLAGS_window << "\" as window "
        << "(should be hexadecimal X ID)";
  }

  int shot_x = 0, shot_y = 0;
  unsigned int shot_width = 0, shot_height = 0;

  Window root_ret = None;
  int x_ret = 0, y_ret = 0;
  unsigned int border_width_ret = 0, depth_ret = 0;
  CHECK(XGetGeometry(display, win,
                     &root_ret,
                     &x_ret, &y_ret,
                     &shot_width, &shot_height,
                     &border_width_ret, &depth_ret));
  if (FLAGS_region) {
    RegionSelector selector(display);
    if (!selector.SelectRegion(&shot_x, &shot_y, &shot_width, &shot_height))
      return 1;
  }

  XImage* image = XGetImage(display, win,
                            shot_x, shot_y,
                            shot_width, shot_height,
                            AllPlanes, ZPixmap);
  CHECK(image);
  CHECK(image->depth == 24 || image->depth == 32)
      << "Unsupported image depth " << image->depth;

  Window visual_feedback_win =
      CreateVisualFeedbackWindow(
          display, shot_x, shot_y, shot_width, shot_height);
  XMapWindow(display, visual_feedback_win);
  XFlush(display);

  usleep(kVisualFeedbackWindowDisplayTimeMs * 1000);
  XDestroyWindow(display, visual_feedback_win);
  XFlush(display);

  cairo_surface_t* surface =
      cairo_image_surface_create_for_data(
          reinterpret_cast<unsigned char*>(image->data),
          image->depth == 24 ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
          image->width,
          image->height,
          image->bytes_per_line);
  CHECK(surface) << "Unable to create Cairo surface from XImage data";
  CHECK(cairo_surface_write_to_png(surface, filename) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(surface);
  XDestroyImage(image);

  XCloseDisplay(display);
  return 0;
}
