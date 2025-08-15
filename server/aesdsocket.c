#include "queue.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
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

int done = 0, sockfd, outfd;
pthread_mutex_t file_mutex;
timer_t timerid;
int file_mutex_initialized = 0, outfd_initialized = 0, timer_initialized = 0;

typedef struct sockthread_data_s sockthread_data_t;
struct sockthread_data_s {
    pthread_t thread;
    int insockfd, status;
    char inaddr[INET6_ADDRSTRLEN];
    SLIST_ENTRY(sockthread_data_s) entries;
};
SLIST_HEAD(sockthread_head, sockthread_data_s);

void join_completed_threads(struct sockthread_head *head) {
    sockthread_data_t *datap = NULL, *prev = NULL;
    SLIST_FOREACH(datap, head, entries) {
        if (datap->status != 0) {
            pthread_join(datap->thread, NULL);
            close(datap->insockfd);
            if (prev == NULL) {
                SLIST_REMOVE_HEAD(head, entries);
            } else {
                SLIST_REMOVE_AFTER(prev, entries);
            }
            free(datap);
        } else {
            prev = datap;
        }
    }
}

void join_all_threads(struct sockthread_head *head) {
    while (!SLIST_EMPTY(head)) {
        sockthread_data_t *datap = SLIST_FIRST(head);
        pthread_join(datap->thread, NULL);
        close(datap->insockfd);
        SLIST_REMOVE_HEAD(head, entries);
        free(datap);
    }
}

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

void timer_handler(union sigval arg) {
    (void)arg;
    // get the time
    time_t curtime;
    memset(&curtime, 0, sizeof curtime);
    time(&curtime);
    struct tm *localts;
    localts = localtime(&curtime);
    if (localts == NULL) {
        syslog(LOG_ERR, "getting current time failed: %s", strerror(errno));
        return;
    }
    char timestr[200];
    strftime(timestr, sizeof timestr, "%a, %d %b %Y %T %z", localts);
    char timefmt[256];
    sprintf(timefmt, "timestamp:%s\n", timestr);

    int err = pthread_mutex_lock(&file_mutex);
    if (err == -1) {
        syslog(LOG_ERR, "failed locking mutex: %s", strerror(errno));
        return;
    }
    size_t timefmt_len = strlen(timefmt);
    ssize_t nwritten = write(outfd, timefmt, timefmt_len);
    if (nwritten != timefmt_len) {
        syslog(LOG_ERR, "failed writing timestamp: %s", strerror(errno));
    }

    err = pthread_mutex_unlock(&file_mutex);
    if (err == -1) {
        syslog(LOG_ERR, "failed to unlock mutex: %s", strerror(errno));
    }
}

int initialize_timer(timer_t *timerid, pthread_mutex_t *mutex) {
    struct sigevent sev;
    struct itimerspec its;

    // create timer
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = &timerid;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;
    if (timer_create(CLOCK_MONOTONIC, &sev, timerid) == -1) {
        syslog(LOG_ERR, "timer_create %s", strerror(errno));
        return -1;
    }

    // start timer
    its.it_value.tv_sec = 10; // 10s
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;
    if (timer_settime(*timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "timer_settime %s", strerror(errno));
        return -1;
    }
    return 0;
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
        off_t offset = 0;
        ssize_t written;
        while (count != 0 &&
               (written = write(outfd, buffer + offset, count)) != -1) {
            offset += written;
            count -= written;
        }
        if (written == -1) {
            syslog(LOG_ERR, "write to file failed with: %s", strerror(errno));
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
            syslog(LOG_ERR, "Error on read from file: %s", strerror(errno));
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
                    syslog(LOG_ERR, "Send failed with %s", strerror(errno));
                    return -1;
                }
            }
            count -= sent;
            offset += sent;
        }
    }
}

// passed in from the linked list
void *sockthread_handler(void *arg) {
    sockthread_data_t *datap = (sockthread_data_t *)arg;
    datap->status = pthread_mutex_lock(&file_mutex);
    if (datap->status == -1) {
        syslog(LOG_ERR, "could not lock file mutex: %s", strerror(errno));
        return arg;
    }
    datap->status = receive_packets(datap->insockfd, outfd, datap->inaddr);
    if (datap->status == -1) {
        datap->status = pthread_mutex_unlock(&file_mutex);
        return arg;
    }
    send_packets(outfd, datap->insockfd);
    datap->status = pthread_mutex_unlock(&file_mutex);
    if (datap->status == -1) {
        syslog(LOG_ERR, "could not lock file mutex: %s", strerror(errno));
        return arg;
    }
    return arg;
}

int initialize_socket() {
    struct addrinfo hints, *servinfo, *p;
    // get the server address
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    int rv;
    if ((rv = getaddrinfo(NULL, "9000", &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    // bind to server address
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) ==
            -1) {
            syslog(LOG_ERR, "Error on socket create %s", strerror(errno));
            continue;
        }
        // Assuming we don't have to reuse the address
        int yes = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) ==
            -1) {
            syslog(LOG_ERR, "setsockopt failed with %s", strerror(errno));
            return -1;
        }
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            syslog(LOG_ERR, "Error on bind %s", strerror(errno));
            continue;
        }
        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        syslog(LOG_ERR, "Server failed to bind");
        return -1;
    }

    return sockfd;
}

int main_run(int argc, char *argv[]) {
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

    openlog("aesdsocket", 0, LOG_USER);
    outfd = open(OUTFILE, O_CREAT | O_RDWR | O_APPEND | O_TRUNC, 0666);
    if (outfd == -1) {
        syslog(LOG_ERR, "Could not create/open output file %s", OUTFILE);
        return -1;
    }
    outfd_initialized = 1;

    // initialize the mutex
    int err = pthread_mutex_init(&file_mutex, NULL);
    if (err != 0) {
        syslog(LOG_ERR, "failed to initialize file mutex: %s", strerror(errno));
        return -1;
    }
    file_mutex_initialized = 1;

    sockfd = initialize_socket();
    if (sockfd == -1) {
        return -1;
    }

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
    
    err = initialize_timer(&timerid, &file_mutex);
    if (err == -1) {
        return -1;
    }
    timer_initialized = 1;

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "Server listen failed");
        return -1;
    }

    // setup term handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Setup signal handler failed with %s", strerror(errno));
        return -1;
    }

    struct sockthread_head head;
    SLIST_INIT(&head);

    while (!done) {
        struct sockaddr_storage their_addr; // connectors address
        socklen_t sin_size = sizeof their_addr;
        int insockfd =
            accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (insockfd == -1) {
            syslog(LOG_ERR, "Accept failed");
            continue;
        }
        sockthread_data_t *datap =
            (sockthread_data_t *)malloc(sizeof(sockthread_data_t));
        datap->status = 0;
        datap->insockfd = insockfd;
        inet_ntop(their_addr.ss_family,
                  get_in_addr((struct sockaddr *)&their_addr), datap->inaddr,
                  sizeof datap->inaddr);

        syslog(LOG_DEBUG, "Accepted connection from %s", datap->inaddr);
        SLIST_INSERT_HEAD(&head, datap, entries);
        // spinoff thread
        pthread_create(&datap->thread, NULL, &sockthread_handler, datap);
        // close any completed threads
        join_completed_threads(&head);
    }
    join_all_threads(&head);
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = main_run(argc, argv);
    // free any initialized resources
    if (timer_initialized) {
        timer_delete(timerid);
    }
    if (file_mutex_initialized) {
        pthread_mutex_destroy(&file_mutex);
    }
    if (outfd_initialized) {
        close(outfd);
    }
    remove(OUTFILE);
    closelog();
    return ret;
}
