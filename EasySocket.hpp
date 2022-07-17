#ifndef EASYSOCKET
#define EASYSOCKET

#ifdef _WIN32
#include <winsock2.h>
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int SOCKET;
const int INVALID_SOCKET = 0;
const int SOCKET_ERROR = -1;
#endif
#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <exception>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <thread>
#include <spdlog/spdlog.h>

///AUX
static int connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen,
        unsigned int timeout_ms=5000) {
    int rc = 0;
    // Set O_NONBLOCK
    int sockfd_flags_before;
    if((sockfd_flags_before=fcntl(sockfd,F_GETFL,0)<0)) return -1;
    if(fcntl(sockfd,F_SETFL,sockfd_flags_before | O_NONBLOCK)<0) return -1;
    // Start connecting (asynchronously)
    do {
        if (connect(sockfd, addr, addrlen)<0) {
            // Did connect return an error? If so, we'll fail.
            if ((errno != EWOULDBLOCK) && (errno != EINPROGRESS)) {
                rc = -1;
            }
                // Otherwise, we'll wait for it to complete.
            else {
                // Set a deadline timestamp 'timeout' ms from now (needed b/c poll can be interrupted)
                struct timespec now;
                if(clock_gettime(CLOCK_MONOTONIC, &now)<0) { rc=-1; break; }
                struct timespec deadline = { .tv_sec = now.tv_sec,
                        .tv_nsec = now.tv_nsec + timeout_ms*1000000l};
                // Wait for the connection to complete.
                do {
                    // Calculate how long until the deadline
                    if(clock_gettime(CLOCK_MONOTONIC, &now)<0) { rc=-1; break; }
                    int ms_until_deadline = (int)(  (deadline.tv_sec  - now.tv_sec)*1000l
                                                    + (deadline.tv_nsec - now.tv_nsec)/1000000l);
                    if(ms_until_deadline<0) { rc=0; break; }
                    // Wait for connect to complete (or for the timeout deadline)
                    struct pollfd pfds[] = { { .fd = sockfd, .events = POLLOUT } };
                    rc = poll(pfds, 1, ms_until_deadline);
                    // If poll 'succeeded', make sure it *really* succeeded
                    if(rc>0) {
                        int error = 0; socklen_t len = sizeof(error);
                        int retval = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
                        if(retval==0) errno = error;
                        if(error!=0) rc=-1;
                    }
                }
                    // If poll was interrupted, try again.
                while(rc==-1 && errno==EINTR);
                // Did poll timeout? If so, fail.
                if(rc==0) {
                    errno = ETIMEDOUT;
                    rc=-1;
                }
            }
        }
    } while(0);
    // Restore original O_NONBLOCK state
    if(fcntl(sockfd,F_SETFL,sockfd_flags_before)<0) return -1;
    // Success
    return rc;
}

namespace masesk {
    const int BUFF_SIZE = 4096;
    struct socket_error_exception : public std::exception
    {
        const char * what() const throw ()
        {
            return "Can't start socket!";
        }
    };
    struct invalid_socket_exception : public std::exception
    {
        const char * what() const throw ()
        {
            return "Can't create a socket!";
        }
    };
    struct data_size_exception : public std::exception
    {
        const char * what() const throw ()
        {
            return "Data size is above the maximum allowed by the buffer";
        }
    };
    class EasySocket {
    public:
        // only one client at a time
        template<class F, typename... Args>
        void socketListen(const std::string &channelName, int port, F&& callback, Args... args){
            static constexpr uint _max_clients = 30;
            static constexpr uint _buffer_size = 2048+1; //2k
            spdlog::debug("EasySocket::socketListen ()");
            int master_socket , addrlen , new_socket , client_socket[_max_clients] ,
             activity, i , valread , sd;
            int max_sd;
            struct sockaddr_in address;
            char buffer[_buffer_size];

            //set of socket descriptors
            fd_set readfds;

            //initialise all client_socket[] to 0 so not checked
            for (i = 0; i < _max_clients; i++){
                client_socket[i] = 0;
            }

            //create a master socket
            if( (master_socket = socket(AF_INET , SOCK_STREAM , 0)) == 0){
                spdlog::error("EasySocket::socketListen() socket failed");
                exit(EXIT_FAILURE);
            }

            //set master socket to allow multiple connections ,
            //this is just a good habit, it will work without this
            int opt = true;
            if( setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 ){
                spdlog::error("EasySocket::socketListen() setsockopt failed");
                exit(EXIT_FAILURE);
            }

            //type of socket created
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons( port );

            //bind the socket to localhost port 8888
            if (bind(master_socket, (struct sockaddr *)&address, sizeof(address))<0){
                spdlog::error("EasySocket::socketListen() bind failed");
                exit(EXIT_FAILURE);
            }
            spdlog::debug("EasySocket::socketListen() Listen on port: {}", port);

            //try to specify maximum of 3 pending connections for the master socket
            if (listen(master_socket, 3) < 0){
                spdlog::error("EasySocket::socketListen() listen failed");
                exit(EXIT_FAILURE);
            }

            //accept the incoming connection
            addrlen = sizeof(address);
            spdlog::debug("EasySocket::socketListen() Waiting for connections");

            while(true){
                //clear the socket set
                FD_ZERO(&readfds);

                //add master socket to set
                FD_SET(master_socket, &readfds);
                max_sd = master_socket;

                //add child sockets to set
                for ( i = 0 ; i < _max_clients ; i++){
                    //socket descriptor
                    sd = client_socket[i];

                    //if valid socket descriptor then add to read list
                    if(sd > 0)
                        FD_SET( sd , &readfds);

                    //highest file descriptor number, need it for the select function
                    if(sd > max_sd)
                        max_sd = sd;
                }

                //wait for an activity on one of the sockets , timeout is NULL ,
                //so wait indefinitely
                activity = select( max_sd + 1 , &readfds , NULL , NULL , NULL);
                if ((activity < 0) && (errno!=EINTR)){
                    spdlog::error("EasySocket::socketListen() select failed");
                }

                //If something happened on the master socket ,
                //then its an incoming connection
                if (FD_ISSET(master_socket, &readfds)){
                    if ((new_socket = accept(master_socket,
                                             (struct sockaddr *)&address, (socklen_t*)&addrlen))<0){
                        spdlog::error("EasySocket::socketListen() accept failed");
                        exit(EXIT_FAILURE);
                    }

                    //inform user of socket number - used in send and receive commands
                    spdlog::debug("EasySocket::socketListen() New connection , socket fd: {} ip: {} "
                                  "port: {}", new_socket , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

                    //add new socket to array of sockets
                    for (i = 0; i < _max_clients; i++){
                        //if position is empty
                        if( client_socket[i] == 0 ){
                            client_socket[i] = new_socket;
                            spdlog::debug("EasySocket::socketListen() Adding to list of sockets as: {}", i);
                            break;
                        }
                    }
                }
                //else its some IO operation on some other socket
                for (i = 0; i < _max_clients; i++){
                    sd = client_socket[i];
                    if (FD_ISSET( sd , &readfds)){
                        //Check if it was for closing , and also read the
                        //incoming message
                        if ((valread = read( sd , buffer, _buffer_size-1)) == 0){
                            //Somebody disconnected , get his details and print
                            getpeername(sd , (struct sockaddr*)&address , (socklen_t*)&addrlen);
                            spdlog::debug("EasySocket::socketListen() Host disconnected, ip: {} port: {}",
                                   inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

                            //Close the socket and mark as 0 in list for reuse
                            close( sd );
                            client_socket[i] = 0;
                        }
                        //Echo back the message that came in
                        else{
                            //set the string terminating NULL byte on the end
                            //of the data read
                            buffer[valread] = '\0';
                            // send(sd , buffer , strlen(buffer) , 0 );
                            callback(std::string(buffer, 0, strlen(buffer) ), args ...);
                        }
                    }
                }
            }
        }


        void socketSend(const std::string &channelName,  const std::string &data) {
            spdlog::debug("EasySocket::socketSend ()");
            if (data.size() > BUFF_SIZE) {
                ///throw masesk::data_size_exception();
                spdlog::error("EasySocket::socketSend () Data size is above the maximum allowed by the buffer.");
                sockQuit();
            }

            if (client_sockets.find(channelName) != client_sockets.end()) {
                SOCKET sock = client_sockets.at(channelName);
                int sendResult = send(sock, data.c_str(), data.size() + 1, 0);
                if (sendResult == SOCKET_ERROR)
                {
                    ///throw masesk::socket_error_exception();
                    spdlog::error("EasySocket::socketSend () Can't start socket! ");
                    sockQuit();
                }
            }
        }

        void socketSendRaw(const std::string &channelName,  const void *data, const int size) {
            spdlog::debug("EasySocket::socketSendRaw ()");
            if (size > BUFF_SIZE) {
                ///throw masesk::data_size_exception();
                spdlog::error("EasySocket::socketSendRaw () Data size is above the maximum allowed by the buffer.");
                sockQuit();
            }

            if (client_sockets.find(channelName) != client_sockets.end()) {
                SOCKET sock = client_sockets.at(channelName);
                int sendResult = send(sock, data, size, 0);
                if (sendResult == SOCKET_ERROR)
                {
                    ///throw masesk::socket_error_exception();
                    spdlog::error("EasySocket::socketSendRaw () Can't start socket! ");
                    sockQuit();
                }
            }
        }

        std::vector<unsigned char> socketSendRecvRaw(const std::string &channelName, const void *data,
                                                     const int writeSize, const int recvSize) {
            spdlog::debug("EasySocket::socketSendRecvRaw ()");
            if (writeSize > BUFF_SIZE) {
                ///throw masesk::data_size_exception();
                spdlog::error("EasySocket::socketSendRecvRaw () Data size is above the maximum allowed by "
                              "the buffer.");
                sockQuit();
            }
            if (client_sockets.find(channelName) != client_sockets.end()) {
                SOCKET sock = client_sockets.at(channelName);
                int sendResult = send(sock, data, writeSize, 0);
                if (sendResult == SOCKET_ERROR){
                    ///throw masesk::socket_error_exception();
                    spdlog::error("EasySocket::socketSendRecvRaw () Can't start send socket! ");
                    sockQuit();
                    return {};
                }

                char tmp[recvSize];
                std::vector<unsigned char> ret;

                ///RECV timeout
                struct timeval tv;
                tv.tv_sec = 1; // 1secs
                tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);


                int bytesReceived = recv(sock, (void*) tmp, recvSize, 0);
                if (bytesReceived == SOCKET_ERROR) {
                    ///throw socket_error_exception();
                    spdlog::error("EasySocket::socketSendRecvRaw () Can't start recv socket! ");
                    sockQuit();
                    return {};

                }
                if (bytesReceived > recvSize) {
                    ///throw masesk::data_size_exception();
                    spdlog::error("EasySocket::socketSendRecvRaw () Data size is above the maximum allowed by "
                                  "the buffer.");
                    sockQuit();
                    return {};

                }
                if (bytesReceived > 0) {
                    for(auto i=0; i< bytesReceived; ++i){
                        ret.emplace_back(tmp[i]);
                    }
                    return ret;
                }
            }
            return {};
        }


        void socketConnect(const std::string &channelName, const std::string &ip, std::uint16_t port) {
            spdlog::debug("EasySocket::socketConnect ()");
            if (sockInit() != 0) {
                ///throw masesk::socket_error_exception();
                spdlog::error("EasySocket::socketConnect () Can't start socket! ");
                sockQuit();
                return;
            }
            SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET || sock == SOCKET_ERROR)
            {
                ///sockQuit();
                ///throw masesk::socket_error_exception();
                spdlog::error("EasySocket::socketConnect () Can't start socket! ");
                sockQuit();
            }
            sockaddr_in hint;
            hint.sin_family = AF_INET;
            hint.sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &hint.sin_addr);
            int connResult = connect(sock, (sockaddr*)&hint, sizeof(hint));
            if (connResult == SOCKET_ERROR)
            {
                sockClose(sock);
                ///sockQuit();
                ///throw socket_error_exception();
                spdlog::error("EasySocket::socketConnect () Can't start socket! ");
                sockQuit();
            }
            client_sockets[channelName] = sock;

        }

        [[nodiscard]] int socketConnectDomain(const std::string &channelName, const std::string &hostname,
                                              std::uint16_t port) {
            spdlog::debug("EasySocket::socketConnectDomain ()");
            if (sockInit() != 0) {
                ///throw masesk::socket_error_exception();
                spdlog::error("EasySocket::socketConnectDomain () Can't start socket! ");
                sockQuit();
                return 0;
            }

            int check_sfd;
            struct addrinfo hints, *p, *servinfo;

            memset(&hints, 0, sizeof(struct addrinfo));
            hints.ai_family = AF_UNSPEC;//AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = 0;
            hints.ai_protocol = 0;

            int res = getaddrinfo(hostname.c_str(), NULL, &hints, &servinfo);
            if (res != 0) {
                ///sockQuit();
                ///throw socket_error_exception();
                spdlog::error("EasySocket::socketConnectDomain () Can't start socket! ");
                sockQuit();
                return 0;
            }

            // getaddrinfo returned a linked list of relevant addresses,
            // loop through the addresses and return the first one available
            for (p = servinfo; p != NULL; p = p->ai_next) {
                check_sfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (check_sfd == -1)
                    continue;

                struct sockaddr_in *sockstruct = (struct sockaddr_in*) p->ai_addr;
                sockstruct->sin_port = htons(port);
                sockstruct->sin_family = AF_INET;
                socklen_t addrsize = sizeof(struct sockaddr_in);

                // successfully connected
                if (connect_with_timeout(check_sfd, (struct sockaddr*)sockstruct, addrsize, 1000) != -1) {
                    client_sockets[channelName] = check_sfd;
                    break;
                }
                close(check_sfd);
            }
            if (p == NULL) { // couldn't connect to any item in the linked list
                ///sockQuit();
                ///throw socket_error_exception();
                spdlog::error("EasySocket::socketConnectDomain () Can't start socket! ");
                sockQuit();
                return 0;
            }
            freeaddrinfo(servinfo);
            return 1;
        }


        std::string socketSendRecv(const std::string &channelName, const std::string & data) {
            spdlog::debug("EasySocket::socketSendRecv ()");

            if (client_sockets.find(channelName) != client_sockets.end()) {
                SOCKET sock = client_sockets.at(channelName);
                int sendResult = send(sock, data.c_str(), data.size() , 0);
                if (sendResult == SOCKET_ERROR){
                    spdlog::error("EasySocket::socketSendRecv () Can't start socket! ");
                    sockQuit();
                    return "";
                }

                char buff[4096];


                ///RECV timeout
                struct timeval tv;
                tv.tv_sec = 1; // 1secs
                tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

                int bytesReceived = recv(sock, buff, BUFF_SIZE, 0);
                if (bytesReceived == SOCKET_ERROR) {
                    spdlog::error("EasySocket::socketSendRecv () Can't start socket! ");
                    sockQuit();
                    return "";
                }
                if (bytesReceived > BUFF_SIZE) {
                    spdlog::error("EasySocket::socketSendRecv () Data size is above the maximum allowed by "
                                  "the buffer.");
                    sockQuit();
                    return "";
                }
                if (bytesReceived > 0) {
                    return std::string(buff, 0, bytesReceived);
                }
            }
            return "";
        }


        void closeConnection(const std::string &channelName) {
            spdlog::debug("EasySocket::closeConnection ()");
            if (client_sockets.find(channelName) != client_sockets.end()) {
                SOCKET s = client_sockets.at(channelName);
                sockClose(s);
                sockQuit();
            }

        }
    private:

        std::unordered_map<std::string, SOCKET> client_sockets;
        std::unordered_map<std::string, SOCKET> server_sockets;
        int sockInit(void)
        {
#ifdef _WIN32
            WSADATA wsa_data;
			return WSAStartup(MAKEWORD(1, 1), &wsa_data);
#else
            return 0;
#endif
        }

        int sockQuit(void)
        {
#ifdef _WIN32
            return WSACleanup();
#else
            return 0;
#endif
        }
        int sockClose(SOCKET sock)
        {

            int status = 0;

#ifdef _WIN32
            status = shutdown(sock, SD_BOTH);
			if (status == 0) { status = closesocket(sock); }
#else
            status = shutdown(sock, SHUT_RDWR);
            if (status == 0) { status = close(sock); }
#endif

            return status;

        }
    };
}

#endif