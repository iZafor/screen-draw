#include "ipc.hpp"
#include "window.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* GS_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys";
static const char* GS_KEY = "custom-keybindings";
static const char* GS_CUSTOM_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding";
static const char* KEYBINDING_SLOT = "screen-draw";
static const char* KEYBINDING_PATH_PREFIX = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/";

std::string ipc_socket_path() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return std::string(rt) + "/screen-draw.sock";
    return "/tmp/screen-draw-" + std::to_string((unsigned long)getuid()) + ".sock";
}

static bool program_on_path(const char* prog) {
    return std::system((std::string("command -v ") + prog + " >/dev/null 2>&1").c_str()) == 0;
}

static void run_cmd(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static std::string capture_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
}

static std::string toggle_command() {
    std::string exe;
    if (const char* appimage = std::getenv("APPIMAGE")) {
        exe = appimage;
    } else {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            exe = buf;
        }
    }
    if (exe.empty()) return "screen-draw --toggle";

    std::string on_path = capture_cmd("command -v screen-draw 2>/dev/null");
    while (!on_path.empty() && (on_path.back() == '\n' || on_path.back() == '\r')) on_path.pop_back();
    if (!on_path.empty()) {
        char a[4096], b[4096];
        if (realpath(on_path.c_str(), a) && realpath(exe.c_str(), b) && std::strcmp(a, b) == 0) {
            return "screen-draw --toggle";
        }
    }
    return shell_quote(exe) + " --toggle";
}

void setup_global_hotkey() {
    if (!program_on_path("gsettings")) return;

    std::string current = capture_cmd(std::string("gsettings get ") + GS_SCHEMA + " " + GS_KEY + " 2>/dev/null");
    if (current.empty()) return; // not a GNOME session / schema missing

    std::string toggle_cmd = toggle_command();
    std::string new_path = std::string(KEYBINDING_PATH_PREFIX) + KEYBINDING_SLOT + "/";
    std::string schema_path = std::string(GS_CUSTOM_SCHEMA) + ":" + new_path;

    if (current.find(KEYBINDING_SLOT) != std::string::npos) {
        run_cmd({"gsettings", "set", schema_path, "command", toggle_cmd});
        std::printf("[\u2713] F9 hotkey already registered\n");
        return;
    }

    std::vector<std::string> paths;
    if (current.find("@as []") == std::string::npos && current.find("[]") == std::string::npos) {
        size_t pos = 0;
        while ((pos = current.find('\'', pos)) != std::string::npos) {
            size_t end = current.find('\'', pos + 1);
            if (end == std::string::npos) break;
            paths.push_back(current.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
    }
    paths.push_back(new_path);

    run_cmd({"gsettings", "set", schema_path, "name", "Screen Draw Toggle"});
    run_cmd({"gsettings", "set", schema_path, "command", toggle_cmd});
    run_cmd({"gsettings", "set", schema_path, "binding", "F9"});

    std::string paths_str = "[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) paths_str += ", ";
        paths_str += "'" + paths[i] + "'";
    }
    paths_str += "]";
    run_cmd({"gsettings", "set", GS_SCHEMA, GS_KEY, paths_str});
    std::printf("[\u2713] F9 hotkey registered via GNOME custom keybindings\n");
}

static bool fill_sockaddr(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool send_ipc_command(const std::string& cmd, const std::string& sock_path) {
    sockaddr_un addr;
    std::string target_path = sock_path.empty() ? ipc_socket_path() : sock_path;
    if (!fill_sockaddr(addr, target_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    std::string msg = cmd + "\n";
    ssize_t sent = send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);
    close(fd);
    return sent == (ssize_t)msg.size();
}

int create_ipc_listener(const std::string& path) {
    sockaddr_un addr;
    if (!fill_sockaddr(addr, path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(path.c_str()); // remove a stale socket from a crashed instance
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0600);
    return fd;
}

void handle_ipc_client(int listen_fd, ScreenDrawWindow& win) {
    for (;;) {
        int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) return;
        timeval tv = {0, 200000};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[64];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        close(cfd);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string cmd(buf);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();

        if (cmd == "toggle") win.toggle_overlay();
        else if (cmd == "show") { if (!win.is_visible) win.show_overlay(); else if (win.passthrough_mode) win.exit_passthrough(); }
        else if (cmd == "hide") { if (win.is_visible) win.hide_overlay(); }
        else if (cmd == "quit") win.quit_requested = true;
    }
}
