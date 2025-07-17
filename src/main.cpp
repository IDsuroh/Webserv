#include "../include/Headers.hpp"

int main()  {
    
    const int   port = 8080;
    int         server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)  {
        perror("socket");
        return 1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)    {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 1) < 0)   {
        perror("listen");
        return 1;
    }

    std::cout
        << "Listening on port " << port << "...\n";

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0)  {
        perror("accept");
        return 1;
    }

    char    buffer[1024];
    read(client_fd, buffer, sizeof(buffer));

    const char* response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello, world!";
    write(client_fd, response, strlen(response));

    close(client_fd);
    close(server_fd);

    return 0;

}