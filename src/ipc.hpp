#pragma once

#include <string>

class ScreenDrawWindow;

std::string ipc_socket_path();
std::string shell_quote(const std::string& s);
void setup_global_hotkey();

bool send_ipc_command(const std::string& cmd, const std::string& sock_path = "");
int create_ipc_listener(const std::string& path);
void handle_ipc_client(int listen_fd, ScreenDrawWindow& win);
