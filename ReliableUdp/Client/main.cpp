#include "ReliableUdp.h"
constexpr uint16_t SERVER_PORT = 6667;

void SendMessage(FILE *fp, int socket_fd, const sockaddr_in *server_address, socklen_t server_address_len) {
    using namespace wjx_udp;
    int n;
    char sendline[MAXLINE];
    ReliableUdpClient reliable_udp_client(socket_fd);
    reliable_udp_client.Connect(socket_fd, server_address, server_address_len);
    /*while (Fgets(sendline, MAXLINE, fp)) {
        size_t message_length = strlen(sendline);
        std::cout << "start_send!\n";
        reliable_udp_client.Send(sendline, message_length, server_address, server_address_len);
        std::cout << "message sent, length = " << message_length << '\n';
    }*/
    for(int i = 0; i < 10; ++i) {
        sendline[0] = i + '0';
        size_t message_length = 1;
        reliable_udp_client.Send(sendline, message_length, server_address, server_address_len);
        printf("\033[;31;40mmessage sent, length = %d\n\033[0m ", message_length);
    }
    getchar();
    printf("xi gou\n");
}

int main(int argc, char **argv) {
    int socket_fd;
    sockaddr_in server_address;
    if (argc != 2) {
        printf("usage: ./client <ipaddress>\n");
        return 0;
    }
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    std::cout << "server ip is :" << argv[1] << '\n';
    if(inet_pton(AF_INET, argv[1], &server_address.sin_addr) < 0){
        printf("inet_pton error\n");
        return 0;
    }

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(socket_fd < 0) {
        printf("socker_error\n");
        return 0;
    }
    SendMessage(stdin, socket_fd, &server_address, sizeof(server_address));
}