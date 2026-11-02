// Screen Draw — Native C++17 / Xlib / Cairo Screen Annotation Overlay
// Fast, hardware-composited, dirty-region optimized UI with movable & resizable shapes.
// No GTK / GLib dependency: talks to the X server (or XWayland) directly via Xlib.

#include "window.hpp"
#include "ipc.hpp"

#include <poll.h>
#include <unistd.h>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>

static int x_error_handler(Display* d, XErrorEvent* e) {
    char text[256];
    XGetErrorText(d, e->error_code, text, sizeof(text));
    std::fprintf(stderr, "[!] X error: %s (request %d.%d)\n", text, e->request_code, e->minor_code);
    return 0; // non-fatal, like GDK
}

static void print_usage(const char* prog) {
    std::printf("Usage: %s [--toggle | --show | --hide | --quit]\n"
                "  (no option)  start in the background, or toggle a running instance\n"
                "  --toggle     toggle the overlay (starts the app if needed)\n"
                "  --show       show the overlay (starts the app if needed)\n"
                "  --hide       hide the overlay of a running instance\n"
                "  --quit       quit a running instance\n", prog);
}

int main(int argc, char** argv) {
    std::string command; // "" = plain launch
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--toggle" || a == "-t") command = "toggle";
        else if (a == "--show") command = "show";
        else if (a == "--hide") command = "hide";
        else if (a == "--quit" || a == "-q") command = "quit";
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { print_usage(argv[0]); return 2; }
    }

    // Forward to an already running instance (a plain launch toggles it)
    if (send_ipc_command(command.empty() ? "toggle" : command)) return 0;
    if (command == "hide" || command == "quit") return 0; // nothing running

    std::printf("┌──────────────────────────────────────────┐\n");
    std::printf("│  Screen Draw — Fast Native C++ Overlay   │\n");
    std::printf("├──────────────────────────────────────────┤\n");
    std::printf("│  F9       Toggle overlay                 │\n");
    std::printf("│  S / V    Select, Move & Resize shapes   │\n");
    std::printf("│  P        Pen tool                       │\n");
    std::printf("│  E        Eraser tool                    │\n");
    std::printf("│  T        Text tool                      │\n");
    std::printf("│  L/R/O/A  Line/Rect/Circle/Arrow         │\n");
    std::printf("│  Del      Delete selected shape          │\n");
    std::printf("│  Ctrl+Z   Undo                           │\n");
    std::printf("│  Ctrl+Y   Redo                           │\n");
    std::printf("│  Ctrl+X   Clear canvas                   │\n");
    std::printf("│  C        Interact mode                  │\n");
    std::printf("│  Esc      Deselect / Close submenu       │\n");
    std::printf("└──────────────────────────────────────────┘\n");
    std::fflush(stdout);

    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGPIPE, SIG_IGN);

    // Locale for XIM-based UTF-8 text input
    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");

    // Connects to XWayland on GNOME Wayland (via $DISPLAY) so the fullscreen
    // transparent overlay can stay always-on-top.
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "[!] Cannot open X display (is $DISPLAY set? XWayland is required on Wayland)\n");
        return 1;
    }
    XSetErrorHandler(x_error_handler);

    std::string sock_path = ipc_socket_path();
    int listen_fd = create_ipc_listener(sock_path);
    if (listen_fd < 0) {
        std::fprintf(stderr, "[!] Could not create control socket at %s; F9 toggling will not work\n", sock_path.c_str());
    } else {
        std::printf("[\u2713] Control socket listening at %s\n", sock_path.c_str());
    }

    setup_global_hotkey();

    auto win = std::make_unique<ScreenDrawWindow>(dpy);
    if (command == "toggle" || command == "show") win->show_overlay();

    int xfd = ConnectionNumber(dpy);
    while (!win->quit_requested) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            win->handle_event(ev);
        }
        win->run_idle();
        win->run_timers();
        win->present_dirty();
        XFlush(dpy);
        if (win->quit_requested) break;
        if (XPending(dpy) > 0) continue;

        pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = {xfd, POLLIN, 0};
        if (listen_fd >= 0) fds[nfds++] = {listen_fd, POLLIN, 0};
        int r = poll(fds, nfds, win->next_timeout_ms());
        if (r > 0 && listen_fd >= 0 && (fds[1].revents & POLLIN)) {
            handle_ipc_client(listen_fd, *win);
        }
    }

    win.reset();
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(sock_path.c_str());
    }
    XCloseDisplay(dpy);
    return 0;
}
