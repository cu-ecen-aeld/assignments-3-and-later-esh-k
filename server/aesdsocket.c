#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define PORT "9000"
#define OUTFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define BUFSZ 512

int done, sockfd;

void sigchld_handler(int s) {
    if (s != SIGINT && s != SIGTERM)
        return;
    syslog(LOG_DEBUG, "Caught signal, exiting");
    shutdown(sockfd, 0); // stop receiving packets but can send to complete any
                         // pending operations
    done = 1;
}

void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int receive_packets(int infd, int outfd, char *inaddr) {
    char buffer[BUFSZ];
    while (1) {
        size_t max_len = sizeof buffer - 1;
        ssize_t count = recv(infd, buffer, max_len, 0);
        if (count == -1) {
            switch (errno) {
            case EAGAIN:
                // retry
                continue;
            default:
                syslog(LOG_ERR, "Error on recv");
                return -1;
            }
        } else if (count == 0) { // connection closed
            write(infd, "\n", 1);
            syslog(LOG_ERR, "Closed connection from %s", inaddr);
            return -1;
        }
        buffer[count] = '\0';
        int is_end_newline = buffer[count - 1] == '\n';
        ssize_t written;
        off_t offset = 0;
        while (count != 0 &&
               (written = write(outfd, buffer + offset, count)) != -1) {
            offset += written;
            count -= written;
        }
        if (written == -1) {
            syslog(LOG_ERR, "write to file failed with: %d", errno);
            return -1;
        }
        if (is_end_newline) {
            return 0;
        }
    }
}

int send_packets(int fd, int sockfd) {
    char buf[BUFSZ];
    size_t bufsize = sizeof buf;
    ssize_t sent = 0;
    lseek(fd, 0, SEEK_SET);
    while (1) {
        ssize_t count = read(fd, buf, bufsize - 1);
        if (count == -1) {
            syslog(LOG_ERR, "Error on read from file: %d", errno);
            return -1;
        }
        if (count == 0) { // completed sending
            return 0;
        }
        buf[count] = '\0';

        off_t offset = 0;
        while (count > 0) {
            if ((sent = send(sockfd, buf + offset, count, 0)) == -1) {
                switch (errno) {
                case EAGAIN:
                case EINTR:
                    continue;
                default:
                    syslog(LOG_ERR, "Send failed with %d", errno);
                    return -1;
                }
            }
            count -= sent;
            offset += sent;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc > 2) {
        printf("Usage: aesdsocket [-d]\n");
        return -1;
    }
    int daemon_mode = 0;
    if (argc == 2) {
        if (strcmp(argv[1], "-d") != 0) {
            printf("Usage: aesdsocket [-d]\n");
            return -1;
        }
        daemon_mode = 1;
    }
    int new_fd, outfd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connectors address
    socklen_t sin_size;
    struct sigaction sa;
    // int yes = 1;
    char inaddr[INET6_ADDRSTRLEN];
    int rv;
    done = 0;

    openlog("aesdsocket", 0, LOG_USER);
    outfd = open(OUTFILE, O_CREAT | O_RDWR | O_APPEND | O_TRUNC, 0666);
    if (outfd == -1) {
        syslog(LOG_ERR, "Could not create/open output file %s", OUTFILE);
        return -1;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, "9000", &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) ==
            -1) {
            syslog(LOG_ERR, "Error on socket create %d", errno);
            continue;
        }
        // Assuming we don't have to reuse the address
        int yes = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) ==
            -1) {
            syslog(LOG_ERR, "setsockopt failed with %d", errno);
            return -1;
        }
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            syslog(LOG_ERR, "Error on bind %d", errno);
            continue;
        }
        break;
    }

    freeaddrinfo(servinfo);
    if (daemon_mode) {
        int pid = fork();
        if (pid != 0) {
            printf("Parent: daemon running in pid: %d\n", pid);
            printf("Parent: closing socket\n");
            close(outfd);
            close(sockfd);
            closelog();
            return 0;
        }
        if (pid < 0) {
            syslog(LOG_ERR, "Daemon creation failed");
            return -1;
        }
    }

    if (p == NULL) {
        syslog(LOG_ERR, "Server failed to bind");
        return -1;
    }

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Server listen failed");
        return -1;
    }

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Setup signal handler failed with %d", errno);
        return -1;
    }

    while (!done) {
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            syslog(LOG_ERR, "Accept failed");
            continue;
        }
        inet_ntop(their_addr.ss_family,
                  get_in_addr((struct sockaddr *)&their_addr), inaddr,
                  sizeof inaddr);

        syslog(LOG_DEBUG, "Accepted connection from %s", inaddr);

        int err = receive_packets(new_fd, outfd, inaddr);
        if (err == -1) {
            close(new_fd);
            return -1;
        }
        err = send_packets(outfd, new_fd);
        close(new_fd);
        if (err == -1) {
            return -1;
        }
    }
    close(outfd);
    remove(OUTFILE);
    closelog();
    return 0;
}
