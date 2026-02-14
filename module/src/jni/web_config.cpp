#include "web_config.h"
#include "log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>

#define PORT 8888
#define CONFIG_PATH "/data/local/tmp/re.zyg.fri/config.json"

static const std::string HTML_DASHBOARD = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ZygiskFrida Config</title>
    <style>
        body { font-family: sans-serif; max-width: 800px; margin: 2rem auto; padding: 0 1rem; }
        textarea { width: 100%; height: 300px; font-family: monospace; }
        button { padding: 10px 20px; font-size: 1.1em; cursor: pointer; }
        .success { color: green; }
        .error { color: red; }
    </style>
</head>
<body>
    <h1>ZygiskFrida Configuration</h1>
    <p>Edit the configuration JSON below and click Save.</p>
    <textarea id="configArea"></textarea>
    <br><br>
    <button onclick="saveConfig()">Save Configuration</button>
    <span id="status"></span>

    <script>
        async function loadConfig() {
            try {
                const response = await fetch('/api/config');
                const text = await response.text();
                document.getElementById('configArea').value = text;
            } catch (e) {
                document.getElementById('status').innerText = 'Error loading config: ' + e;
                document.getElementById('status').className = 'error';
            }
        }

        async function saveConfig() {
            const config = document.getElementById('configArea').value;
            try {
                // Validate JSON
                JSON.parse(config);
                
                const response = await fetch('/api/config', {
                    method: 'POST',
                    body: config
                });
                
                if (response.ok) {
                    document.getElementById('status').innerText = 'Saved successfully!';
                    document.getElementById('status').className = 'success';
                } else {
                    document.getElementById('status').innerText = 'Error saving: ' + response.statusText;
                    document.getElementById('status').className = 'error';
                }
            } catch (e) {
                document.getElementById('status').innerText = 'Invalid JSON: ' + e;
                document.getElementById('status').className = 'error';
            }
        }

        loadConfig();
    </script>
</body>
</html>
)";

std::string read_config() {
    std::ifstream f(CONFIG_PATH);
    if (!f.is_open()) return "{}";
    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
}

void save_config(const std::string& content) {
    std::ofstream f(CONFIG_PATH);
    if (f.is_open()) {
        f << content;
    }
}

void handle_client(int client_socket) {
    char buffer[4096] = {0};
    read(client_socket, buffer, 4096);
    std::string request(buffer);

    std::string response;
    
    if (request.find("GET / ") != std::string::npos || request.find("GET /index.html") != std::string::npos) {
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n" + HTML_DASHBOARD;
    } else if (request.find("GET /api/config") != std::string::npos) {
        std::string config_content = read_config();
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + config_content;
    } else if (request.find("POST /api/config") != std::string::npos) {
        size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            std::string body = request.substr(body_pos + 4);
            // Very basic body reading - usually Content-Length should be checked but assuming small JSON for now
            // If body is empty or cut off, we might need to read more from socket, but for this simple dashboard usually fine.
            // Let's assume the buffer captured it all or most of it. fixing large configs might need a loop.
            
            // Check for Content-Length header to be safer
            size_t cl_pos = request.find("Content-Length: ");
            if (cl_pos != std::string::npos) {
                int content_length = std::stoi(request.substr(cl_pos + 16));
                if (body.length() < content_length) {
                     // Read the rest
                     int remaining = content_length - body.length();
                     std::vector<char> extra_buf(remaining);
                     read(client_socket, extra_buf.data(), remaining);
                     body.append(extra_buf.data(), remaining);
                }
            }
            
            save_config(body);
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nConfig Updated";
        } else {
            response = "HTTP/1.1 400 Bad Request\r\n\r\nMissing Body";
        }
    } else {
        response = "HTTP/1.1 404 Not Found\r\n\r\nNot Found";
    }

    send(client_socket, response.c_str(), response.length(), 0);
    close(client_socket);
}

void server_loop() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        LOGE("WebConfig: Socket failed");
        return;
    }

    // Forcefully attaching socket to the port 8888
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        LOGE("WebConfig: Setsockopt failed");
        return;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to 0.0.0.0
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        // This is expected if another process (another app with this module) already bound the port.
        // We just exit silently in that case, implementing a "first-one-wins" leader election.
        LOGD("WebConfig: Port %d busy, skipping web server in this process.", PORT);
        close(server_fd);
        return;
    }

    if (listen(server_fd, 3) < 0) {
        LOGE("WebConfig: Listen failed");
        return;
    }

    LOGI("WebConfig: Server started on port %d", PORT);

    while (true) {
        int new_socket;
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            LOGE("WebConfig: Accept failed");
            continue;
        }
        std::thread(handle_client, new_socket).detach();
    }
}

void start_web_server() {
    std::thread(server_loop).detach();
}
