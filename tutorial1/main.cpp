#include "Networking/Sockets/ListeningSocket.hpp"

int main()  {
    std::cout
        << "Starting...\n";
    std::cout
        << "Binding Socket...\n";
    wsrv::BindingSocket bindsock = wsrv::BindingSocket(AF_INET, SOCK_STREAM, 0, 81, INADDR_ANY);
    (void)bindsock;
    std::cout
        << "Listening Socket...\n";
    wsrv::ListeningSocket   listensock = wsrv::ListeningSocket(AF_INET, SOCK_STREAM, 0, 80, INADDR_ANY, 10);
    (void)listensock;
    std::cout
        << "Success!\n";
}