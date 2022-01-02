#include "../Client//ReliableUdp.h"
using namespace wjx_udp;
constexpr uint16_t SERVER_PORT = 6667;

void ReceiveMessage(int socket_fd, sockaddr *client_address, socklen_t client_length) {

    int n;
    socklen_t length;
    char message[MAXLINE];
    std::cout << "udp server_started" << std::endl;
    ReliableUdpServer reliable_udp_server;
    for (;;) {
        length = client_length;
        n = reliable_udp_server.Receive(socket_fd, message, client_address, &length);
        message[n] = '\0';
        if(n > 0) {
            printf("\033[;31;40mreceive message from client:\n%s\n\033[0m\n", message);
        } else {
            printf("ignore!\n");
        }
    }
}

int main() {
    int socket_fd;
    sockaddr_in server_address, client_address;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(socket_fd < 0) {
        printf("socker_error\n");
        return 0;
    }
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERVER_PORT);
    if(bind(socket_fd, (sockaddr *)&server_address, sizeof(server_address)) < 0) {
        printf("bind error\n");
        return 0;
    }
    ReceiveMessage(socket_fd, (sockaddr *) &client_address, sizeof(client_address));
}