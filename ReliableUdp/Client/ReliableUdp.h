#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <endian.h>

#include <iostream>
#include <set>
#include <mutex>
#include <functional>
#include <thread>
#include <random>
#include <map>
#include <ctime>
#include <chrono>
#include <list>
#include <condition_variable>

namespace wjx_udp {
    /**
    * ack :
    * ack_num : 32bit
    * client_send_time:64bit
    * message :
    * seq_num: 32bit
    * send_time: 64bit
    * data: left of all
    *
    * send_time主要爲了計算RTT更方便
    *
    *
    *
    * */


    uint64_t GetCurrentTimeMilis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

    constexpr uint16_t MAXLINE = 4096;


    void MySendto(int fd, const void *ptr, size_t nbytes, int flags,
                  const struct sockaddr *sa, socklen_t salen) {
        int n;
        while ((n = sendto(fd, ptr, nbytes, flags, sa, salen)) != (ssize_t) nbytes) {
            perror(strerror(errno));
            sleep(1);
        }

    }

    __ssize_t MyRecvfrom(int fd, void *ptr, size_t nbytes, int flags,
                         struct sockaddr *sa, socklen_t *salenptr) {
        ssize_t n;
        n = recvfrom(fd, ptr, nbytes, flags, sa, salenptr);
        if (n < 0) {
            if (errno == EINTR) {
                printf("recvfrom timeout\n");
            }
            printf("recvfrom error");
        }
        return (n);
    }

    struct Message {
        uint32_t message_id;
        std::vector<uint8_t> message;

        bool operator<(const Message &rhs) const {
            return message_id < rhs.message_id;
        }
        bool operator < (const uint32_t id) const{
            return message_id < id;
        }
    };

    struct SendMessage : Message {
        uint64_t resend_time;
        int socket_fd;
        uint8_t resend_times = 0;
        sockaddr_in server_sock_addr;
    };

    struct Connect {
        Connect(int socket_fd, sockaddr_in client_sock_addr, uint32_t received_id) : socket_fd(socket_fd),
                                                                                     client_sock_addr(
                                                                                             client_sock_addr),
                                                                                     ack_id(received_id) {}

        int socket_fd;
        sockaddr_in client_sock_addr;
        mutable uint32_t ack_id;//期待收到
        mutable uint32_t receiving_id;//已知收到的（期待）
        uint64_t connect_start_time;

        bool operator<(const Connect &rhs) const {
            return client_sock_addr.sin_addr.s_addr < rhs.client_sock_addr.sin_addr.s_addr ||
                   (client_sock_addr.sin_addr.s_addr == rhs.client_sock_addr.sin_addr.s_addr
                    && client_sock_addr.sin_port < rhs.client_sock_addr.sin_port);
        }
    };

    class ReliableUdpServer {
    public:
        ssize_t Receive(int socket_fd, char *buffer, sockaddr *client_address,
                        socklen_t *client_address_length) {
            //如果之前收到的包中含有可接收
            if (!waiting_to_get_set.empty()) {
                auto now_connect = *waiting_to_get_set.begin();
                auto connect_iter = connect_set.find(now_connect);
                auto & received_message_of_this_connect = received_message[*connect_iter];
                if (connect_iter != connect_set.end() && !received_message_of_this_connect.empty() &&
                    received_message_of_this_connect.begin()->message_id == connect_iter->receiving_id) {
                    printf("prev_received = %d\n", received_message_of_this_connect.begin()->message_id);
                    uint32_t message_size = received_message_of_this_connect.begin()->message.size();
                    memcpy(buffer, &received_message_of_this_connect.begin()->message[0], message_size);
                    received_message_of_this_connect.erase(received_message_of_this_connect.begin());
                    ++connect_iter->receiving_id;
                    return message_size;
                } else {
                    printf("drop begin of waiting_set\n");
                    waiting_to_get_set.erase(waiting_to_get_set.begin());
                }
            }
            //数据包头部信息：4字节顺序号，8字节发送时间
            char actual_receive_buffer[MAXLINE + 12];
            ssize_t n = MyRecvfrom(socket_fd, actual_receive_buffer, MAXLINE, 0, client_address, client_address_length);
            Connect now_connect = Connect(socket_fd, *reinterpret_cast<sockaddr_in *>(client_address), 0);
            auto connect_iter = connect_set.find(now_connect);
            uint32_t sequence_num;
            uint64_t package_send_time;
            memcpy(&sequence_num, actual_receive_buffer, 4);
            sequence_num = ntohl(sequence_num);
            memcpy(&package_send_time, actual_receive_buffer + 4, 8);
            package_send_time = be64toh(package_send_time);
            //顺序号为0的新连接
            if (connect_iter == connect_set.end()) {
                if (sequence_num == 0) {
                    now_connect.connect_start_time = package_send_time;
                    NewConnect(now_connect);
                } else {
                    return 0;
                }
                connect_iter = connect_set.find(now_connect);
                connect_iter->ack_id++;
                UpdateAckId(connect_iter);
                connect_iter->receiving_id++;
                SendAck(*connect_iter, package_send_time);
                memcpy(buffer, actual_receive_buffer + 12, n - 12);
                return n - 12;
            } else {//已存在的连接
                // drop old package;
                if (package_send_time < connect_iter->connect_start_time) {
                    printf("droped, package_send_time = %lu, connect_start_time = %lu\n",
                           package_send_time, connect_iter->connect_start_time);
                    return 0;
                }
                //重新连接
                if (sequence_num == 0) {
                    now_connect.connect_start_time = package_send_time;
                    NewConnect(now_connect);
                }
                connect_iter = connect_set.find(now_connect);
                //接收数据包
                if (sequence_num == connect_iter->ack_id) {
                    connect_iter->ack_id++;
                    UpdateAckId(connect_iter);
                    connect_iter->receiving_id++;
                    SendAck(*connect_iter, package_send_time);
                    memcpy(buffer, actual_receive_buffer + 12, n - 12);
                    return n - 12;
                } else {//接收到后发的数据包，写入缓存
                    printf("sequence_num %u is not equal\n", sequence_num);
                    //receive only if the package is belong to this connection;
                    if (sequence_num > connect_iter->ack_id) {
                        printf("received sequence_num > ack_id, which is %u, waiting ack is %u\n", sequence_num,
                               connect_iter->ack_id);
                        //超出窗口大小
                        if (received_message[*connect_iter].size() >= MAX_WINDOW_WIDTH) {
                            printf("as the window width is equal to MAX_WINDOW_WIDTH, drop the package");
                        } else {
                            AddMessage(*connect_iter ,sequence_num, actual_receive_buffer + 12, n - 12);
                        }
                    }

                    SendAck(*connect_iter, package_send_time);
                    return 0;
                }
            }
        }

    private:
        std::set<Connect> connect_set, waiting_to_get_set;
        std::map<Connect,std::set<Message>> received_message;
        constexpr static uint32_t MAX_WINDOW_WIDTH = 10000;

        void UpdateAckId(std::set<Connect>::iterator connect_iter) {
            bool updated = false;
            for (const auto &message : received_message[*connect_iter]) {
                if (connect_iter->ack_id == message.message_id) {
                    ++connect_iter->ack_id;
                    updated = true;
                } else {
                    break;
                }
            }
            if (updated) {
                printf("updated\n");
                waiting_to_get_set.insert(*connect_iter);
            }
        }

        void AddMessage(const Connect& connect ,uint32_t pacakge_id, const char *buffer, size_t nbytes) {
            Message message;
            message.message.resize(nbytes);
            memcpy(&message.message[0], buffer, nbytes);
            message.message_id = pacakge_id;
            received_message[connect].insert(message);
        }

        void NewConnect(const Connect &connect) {
            connect.ack_id = 0;
            connect.receiving_id = 0;
            printf("create a new connect, connect_start_time = %lu\n", connect.connect_start_time);
            connect_set.erase(connect);
            connect_set.insert(connect);
        }

        void SendAck(const Connect &connect, uint64_t package_send_time) {
            printf("send_ack = %u, package_send_time =  = %lu\n", connect.ack_id - 1, package_send_time);
            char ack_buffer[12];
            uint32_t n_ack_id = ntohl(connect.ack_id - 1);
            uint64_t n_package_send_time = htobe64(package_send_time);
            memcpy(ack_buffer, &n_ack_id, 4);
            memcpy(ack_buffer + 4, &n_package_send_time, 8);
            MySendto(connect.socket_fd, ack_buffer, 12, 0,
                     reinterpret_cast<const sockaddr *>(&connect.client_sock_addr),
                     sizeof(connect.client_sock_addr));
        }


    };

    class ReliableUdpClient {


    public:
        ReliableUdpClient(int socket_fd) : cv_lock(cv_mutex), socket_fd(socket_fd) {
            static std::default_random_engine random_engine;
            static std::uniform_int_distribution<uint32_t> distribution(0, std::numeric_limits<uint32_t>::max());
            sequence_num = 0;
            start_time = GetCurrentTimeMilis();
            rtt = init_rtt;
            dev_rtt = 0;
            background_resend_thread = std::thread(&ReliableUdpClient::Resend, this);
        }

        ~ReliableUdpClient() {
            resending = false;
            receiving = false;
            cv_resend.notify_all();
            if (background_resend_thread.joinable()) {
                background_resend_thread.detach();
            }
            if (background_recv_thread.joinable()) {
                background_recv_thread.detach();
            }
        }

        void StartResendThread() {
            cv_resend.notify_all();
        }

        void Receive() {
            char receive_ack_buffer[12];
            while (receiving) {
                printf("waiting for ack\n");
                auto n = MyRecvfrom(socket_fd, receive_ack_buffer, MAXLINE, 0, NULL, NULL);
                printf("received something, length = %d\n", n);
                uint32_t ack_num;
                uint64_t package_send_time;
                memcpy(&ack_num, receive_ack_buffer, 4);
                ack_num = ntohl(ack_num);
                memcpy(&package_send_time, receive_ack_buffer + 4, 8);
                package_send_time = be64toh(package_send_time);
                printf("ack_num = %u, package_send_time = %lu\n", ack_num, package_send_time);
                if (package_send_time < start_time) {
                    // old package, just drop it
                    printf("start_time = %lu\n", start_time);
                    printf("old_package, just drop it!\n");
                    continue;
                }
                printf("Current = %lu\n", GetCurrentTimeMilis()) << '\n';
                {
                    std::lock_guard<std::mutex> no_ack_set_lock(no_ack_set_mutex);

                    if (n == 12 && !no_ack_sequence_num_set.empty() && *no_ack_sequence_num_set.begin() <= ack_num) {
                        //receive ack
                        printf("receive ack, ack_num = %d, package_send_time = %lu\n", ack_num, package_send_time);
                        uint32_t sample_rtt = GetCurrentTimeMilis() - package_send_time;
                        printf("sample_rtt = %u\n", sample_rtt);
                        rtt = round((1 - alpha) * rtt) + round(alpha * sample_rtt);
                        dev_rtt = round((1 - beta) * dev_rtt) + round(beta * GetInterval(sample_rtt, rtt));
                        printf("rtt = %u, dev_rtt = %u\n", rtt, dev_rtt);
                        //清除未收到ack的包
                        for (auto iter = no_ack_sequence_num_set.begin(); iter != no_ack_sequence_num_set.end();) {
                            if (*iter <= ack_num) {
                                printf("drop waiting_ack = %u\n", *iter);
                                iter = no_ack_sequence_num_set.erase(iter);
                            } else {
                                break;
                            }
                        }//清除要重发的包
                        for(auto iter = no_ack_message_list.begin(); iter != no_ack_message_list.end();) {
                            if(iter->message_id <= ack_num) {
                                iter = no_ack_message_list.erase(iter);
                            } else {
                                break;
                            }
                        }
                        continue;
                    } else {
                  //      if (no_ack_sequence_num_set.find(ack_num + 1) != no_ack_sequence_num_set.end()) {
                            if(!no_ack_message_list.empty() && no_ack_message_list.begin()->message_id == ack_num + 1) {//?

                                printf("wrong order, resend!\n");
                                MySendto(no_ack_message_list.begin()->socket_fd, &no_ack_message_list.begin()->message[0], no_ack_message_list.begin()->message.size(),
                                         0,
                                         reinterpret_cast<const sockaddr *>(&no_ack_message_list.begin()->server_sock_addr),
                                         sizeof(no_ack_message_list.begin()->server_sock_addr));
                            }
                //        }
                        printf("old ack, droped\n");
                    }
                }
            }
            printf("Receive Thread exit\n");
        }

        void Resend() {
            printf("background_resend_thread started\n");
            char buffer[MAXLINE + 12];
            while (resending) {
                while (resend_message_list.empty()) {
                    cv_resend.wait(cv_lock);
                    if(!resending) {
                        return;
                    }
                }
                if(!resending) {
                    return;
                }
                printf("resend_message_list is not empty\n");
                while (!resend_message_list.empty()) {
                    const SendMessage &first_message = resend_message_list.front();
                    int32_t time_interval;
                    int64_t now_time = GetCurrentTimeMilis();
                    time_interval = first_message.resend_time - now_time;
                    time_interval = std::max(0, time_interval);

                    printf("time_interval = %d\n", time_interval);
                    usleep(time_interval * 1000);
                    std::unique_lock<std::mutex> no_ack_set_lock(no_ack_set_mutex);
                    if (no_ack_sequence_num_set.find(first_message.message_id) == no_ack_sequence_num_set.end()) {
                        printf("message %u _is_received, so resend cancel\n", first_message.message_id);
                        no_ack_set_lock.unlock();
                    } else {
                        no_ack_set_lock.unlock();
                        printf("resend message, message_id = %u, resend_times = %u\n", first_message.message_id,
                               first_message.resend_times + 1);
                        printf("message_size = %zu\n", first_message.message.size());
                        {
                            std::lock_guard socket_fdlock(socket_fdmutex);

                            MySendto(first_message.socket_fd, &first_message.message[0], first_message.message.size(),
                                     0,
                                     reinterpret_cast<const sockaddr *>(&first_message.server_sock_addr),
                                     sizeof(first_message.server_sock_addr));
                        }
                        if (first_message.resend_times + 1 < MAX_RESEND_TIME) {
                            SendMessage message = first_message;
                            rtt *= 1.5;
                            message.resend_time = message.resend_time + rtt + 4 * dev_rtt;
                            message.resend_times = first_message.resend_times + 1;
                            resend_message_list.push_back(message);
                        } else {
                            printf("tried too many times, giveup\n");
                        }
                    }
                    resend_message_list.pop_front();
                }
                printf("resend_message_list is empty\n");
            }
            printf("Resend Thread exit\n");
        }


        void AddToResendSet(uint32_t package_num, int socket_fd, const sockaddr_in *server_addr, const char *buffer,
                            size_t nbytes) {
            SendMessage message;
            message.message_id = package_num;
            message.message.resize(nbytes);
            message.resend_time = GetCurrentTimeMilis() + rtt + 4 * dev_rtt;
            message.socket_fd = socket_fd;
            message.server_sock_addr = *server_addr;
            no_ack_sequence_num_set.insert(package_num);
            memcpy(&message.message[0], buffer, nbytes);
            no_ack_message_list.push_back(message);
        }

        void AddToResendList(uint32_t package_num, int socket_fd, const sockaddr_in *server_addr, const char *buffer,
                             size_t nbytes) {
            SendMessage message;
            message.message_id = package_num;
            message.message.resize(nbytes);
            message.resend_time = GetCurrentTimeMilis() + rtt + 4 * dev_rtt;
            message.socket_fd = socket_fd;
            message.server_sock_addr = *server_addr;
            no_ack_sequence_num_set.insert(package_num);
            memcpy(&message.message[0], buffer, nbytes);
            resend_message_list.push_back(message);
            no_ack_message_list.push_back(message);
            if (resend_message_list.size() == 1) {
                StartResendThread();
            }
        }

        void Connect(int socket_fd, const sockaddr_in *server_address,
                     socklen_t server_address_length) {
            printf("start connect!\n");
            char actual_send_buffer[MAXLINE + 12];
            //发送连接包
            CreateActualPackage(NULL, actual_send_buffer, 0);
            MySendto(socket_fd, actual_send_buffer, 12, 0,
                     reinterpret_cast<const sockaddr *>(server_address),
                     server_address_length);
            AddToResendList(sequence_num, socket_fd, server_address, actual_send_buffer, 12);
            char receive_ack_buffer[12];
            do {
                printf("waiting for connect ack\n");
                auto n = MyRecvfrom(socket_fd, receive_ack_buffer, MAXLINE, 0, NULL, NULL);
                printf("received something, length = %d\n", n);
                uint32_t ack_num;
                uint64_t package_send_time;
                memcpy(&ack_num, receive_ack_buffer, 4);
                ack_num = ntohl(ack_num);
                memcpy(&package_send_time, receive_ack_buffer + 4, 8);
                package_send_time = be64toh(package_send_time);
                printf("ack_num = %u, package_send_time = %lu\n", ack_num, package_send_time);
                //先前的包丢弃
                if (package_send_time < start_time) {
                    // old package, just drop it
                    printf("start_time = %lu\n", start_time);
                    printf("old_package, just drop it!\n");
                    continue;
                }
                std::cout << "Current = " << GetCurrentTimeMilis() << '\n';
                {
                    std::lock_guard<std::mutex> no_ack_set_lock(no_ack_set_mutex);
                    if (n == 12 && no_ack_sequence_num_set.find(ack_num) != no_ack_sequence_num_set.end()) {
                        //receive ack
                        printf("receive ack, ack_num = %d, package_send_time = %lu\n", ack_num, package_send_time);
                        uint32_t sample_rtt = GetCurrentTimeMilis() - package_send_time;
                        printf("sample_rtt = %u\n", sample_rtt);
                        rtt = round((1 - alpha) * rtt) + round(alpha * sample_rtt);
                        dev_rtt = round((1 - beta) * dev_rtt) + round(beta * GetInterval(sample_rtt, rtt));
                        printf("rtt = %u, dev_rtt = %u\n", rtt, dev_rtt);
                        //累计确认
                        for (auto iter = no_ack_sequence_num_set.begin(); iter != no_ack_sequence_num_set.end();) {
                            if (*iter <= ack_num) {
                                iter = no_ack_sequence_num_set.erase(iter);
                            } else {
                                break;
                            }
                        }
                        break;
                    }
                }
            } while (true);
            printf("\033[;31;40mConnect Success!\n\033[0m ");
            printf("--------------------------------------------------------------------\n");
            background_recv_thread = std::thread(&ReliableUdpClient::Receive, this);
        }

        void Send(const char *send_buffer, size_t send_buffer_bytes, const sockaddr_in *server_address,
                  socklen_t server_address_length) {
            char actual_send_buffer[MAXLINE + 12];
            char receive_ack_buffer[12];
            ssize_t n;
            CreateActualPackage(send_buffer, actual_send_buffer, send_buffer_bytes);

            printf("sending package, sequence_num = %d, sendtime = %lu\n", sequence_num, GetCurrentTimeMilis());
            {
                // 供亂序測試使用,把下面的注釋掉，用這個就可以測試亂序了
//                if (sequence_num != 5) {
//                    std::lock_guard socket_fdlock(socket_fdmutex);
//                    MySendto(socket_fd, actual_send_buffer, send_buffer_bytes + 12, 0,
//                             reinterpret_cast<const sockaddr *>(server_address),
//                             server_address_length);
//                    AddToResendList(sequence_num, socket_fd, server_address, actual_send_buffer,
//                                    send_buffer_bytes + 12);
//
//                } else {
//                    std::thread t1 = std::thread([=]() {
//                        sleep(10);
//                        printf("finally send 5th package\n");
//                        std::lock_guard socket_fdlock(socket_fdmutex);
//                        MySendto(socket_fd, actual_send_buffer, send_buffer_bytes + 12, 0,
//                                 reinterpret_cast<const sockaddr *>(server_address),
//                                 server_address_length);
//                        printf("sequence_num = %u\n", 5);
//                        AddToResendList(5, socket_fd, server_address, actual_send_buffer,
//                                        send_buffer_bytes + 12);
//                    });
//                    t1.detach();
//                    AddToResendSet(sequence_num, socket_fd, server_address, actual_send_buffer,
//                                    send_buffer_bytes + 12);
//                }
                {
                    std::lock_guard socket_fdlock(socket_fdmutex);
                    MySendto(socket_fd, actual_send_buffer, send_buffer_bytes + 12, 0,
                             reinterpret_cast<const sockaddr *>(server_address),
                             server_address_length);
                    AddToResendList(sequence_num, socket_fd, server_address, actual_send_buffer,
                                    send_buffer_bytes + 12);
                }

            }
            IncreaseSequenceNum();
        }

    private:
        void CreateActualPackage(const char *send_buffer, char *actual_send_buffer, size_t send_buffer_nbyte) {
            uint32_t n_sequence_num = htonl(sequence_num);
            uint64_t send_time = GetCurrentTimeMilis();
            uint64_t n_send_time = htobe64(send_time);
            memcpy(actual_send_buffer, &n_sequence_num, 4);
            memcpy(actual_send_buffer + 4, &n_send_time, 8);
            memcpy(actual_send_buffer + 12, send_buffer, send_buffer_nbyte);
        }

        void IncreaseSequenceNum() {
            if (sequence_num == std::numeric_limits<uint32_t>::max()) {
                sequence_num = 0;
            } else {
                sequence_num++;
            }
        }

        uint64_t GetInterval(uint32_t lhs, uint32_t rhs) {
            if (lhs >= rhs) {
                return lhs - rhs;
            } else {
                return rhs - lhs;
            }
        }

        std::list<SendMessage> resend_message_list;
        std::list<SendMessage> no_ack_message_list;
            std::set<uint32_t> no_ack_sequence_num_set;
        std::mutex socket_fdmutex, no_ack_set_mutex;
        std::thread background_resend_thread, background_recv_thread;
        std::condition_variable cv_resend;
        std::mutex cv_mutex;
        std::unique_lock<std::mutex> cv_lock;
        bool resending = true;
        bool receiving = true;
        constexpr static uint32_t init_rtt = 200; //ms
        constexpr static double alpha = 0.125;
        constexpr static double beta = 0.25;
        constexpr static uint8_t MAX_RESEND_TIME = 16;
        int socket_fd;
        uint32_t sequence_num = 0;
        uint64_t start_time;
        uint32_t rtt;
        uint32_t dev_rtt;
    };


}  // namespace wjx_udp