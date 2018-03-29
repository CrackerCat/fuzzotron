/*
 * File:   sender.c
 * Author: DoI
 *
 * Methods to send buffers down sockets
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <fcntl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "callback.h"
#include "fuzzotron.h"
#include "sender.h"
#include "util.h"

extern int errno;

/*
 * send a char array down a
 * udp socket
*/
int send_udp(char * host, int port, char * packet, unsigned long packet_len){
    int sock = 0;
    ssize_t r;
    struct sockaddr_in serv_addr;

    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
        fatal("[!] Error: Could not create socket: %s\n", strerror(errno));
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &serv_addr.sin_addr);

    callback_pre_send(sock, packet, packet_len); // user defined callback

    // payload is larger than maximum datagram, send as multiple datagrams
    if(packet_len > 65507){
        const void * position = packet;
        unsigned long rem = packet_len;

        while(rem > 0){
            if(rem > 65507){
                r = sendto(sock,position,65507,0,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
            }
            else{
                r = sendto(sock,position,rem,0,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
            }

            if(r < 0){
                printf("[!] Error: in chunked sendto(): %s\n", strerror(errno));
                close(sock);
                return -1;
            }

            rem -= r;
            position += r;
        }
    }
    else{
        r = sendto(sock,packet,packet_len,0,(struct sockaddr *)&serv_addr,sizeof(serv_addr));

        if(r < 0){
            printf("[!] Error: in sendto(): %s\n", strerror(errno));
            close(sock);
            return -1;
        }
    }

    callback_post_send(sock); // user defined callback
    close(sock);
    return 0;
}

/*
 *	send a char array down a
 *	tcp socket.
*/
int send_tcp(char * host, int port, char * packet, unsigned long packet_len){
    int sock = 0;
    struct sockaddr_in serv_addr;

	if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		fatal("[!] Error: Could not create socket: %s\n", strerror(errno));
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &serv_addr.sin_addr);

    int c = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
	if(c < 0){
		printf("[!] Error: Could not connect: %s errno: %d\n", strerror(errno), errno);
        if(errno == ECONNRESET){
            close(sock);
            return 0; // just skip this testcase
        }
        else{
            close(sock);
		    return -1;
        }
	}

    fcntl(sock, F_SETFL, O_RDONLY|O_NONBLOCK);
	
    callback_pre_send(sock, packet, packet_len); // user defined callback
    
    if(fuzz.is_tls){
        // Set up the things for TLS
        int ret;
        SSL *ssl;
        SSL_CTX * ctx;

        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();

        ctx = SSL_CTX_new(SSLv23_client_method());
        if(ctx == NULL){
            printf("[!] Error spawning TLS context\n");
            ERR_print_errors_fp(stdout);
            return -1;
        }

        ssl = SSL_new(ctx);
        SSL_set_fd(ssl,sock);
        ret = SSL_connect(ssl);
        if (ret < 1){
            printf("[!] Error initiating TLS session. Error no: %d\n", SSL_get_error(ssl, ret));
			SSL_free(ssl);
			close(sock);
			SSL_CTX_free(ctx);
            return -1;
        }

        if(SSL_write(ssl, packet, packet_len)<0){
            printf("[!] Error: SSL_write() error no: %d\n", SSL_get_error(ssl, ret));
        }

        SSL_free(ssl);
        callback_post_send(sock); // user defined callback
        close(sock);
        SSL_CTX_free(ctx);
        return 0;
    }
    else{
        if(write(sock, packet, packet_len) < 0){
                printf("[!] Error: write() error: %s errno: %d\n", strerror(errno), errno);
        }
    }

    callback_post_send(sock); // user defined callback

    if(fuzz.destroy){
        destroy_socket(sock);
    }
    else{
        close(sock);
    }

    return 0;
}

// place the connection in TCP_REPAIR mode and call close(). This will
// immediately destroy the socket.
void destroy_socket(int sock){
    int a = 1;
    if(setsockopt(sock, SOL_TCP, TCP_REPAIR, &a, sizeof(a)) < 0 ){
        // if EPERM then other side likely closed, if BADF then we already closed it
        if(errno == EBADF || errno == EPERM){
            close(sock);
            return;
        }
        printf("[!] destroy_socket: TCP_REPAIR enable failed: %s\n", strerror(errno));
    }

    usleep(100); // there is some weirdness with TCP_REPAIR, need to wait before closing.
    close(sock);
}
