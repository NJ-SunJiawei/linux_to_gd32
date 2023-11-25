#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include "ogs-core.h"

#define QUEUE_SIZE   128
#define MAX_PKT_LEN  1024

typedef struct udp_address_s{
    char    ip[64];
	uint16_t port;
}udp_address_t;

char SERIAL_PORT[128] = "/dev/ttyUSB0";

udp_address_t udp_address = {"192.168.204.128", 10086}; 
udp_address_t dst_udp_address = {"192.168.204.102", 10087}; 

ogs_socket_t  usb_fd = -1;
ogs_sock_t *sock = NULL;
ogs_socknode_t *node = NULL;
ogs_socknode_t *dst_node = NULL;

static ogs_thread_t *thread = NULL;
static ogs_pollset_t *pollset = NULL;
static ogs_poll_t *uart_rx_poll = NULL;
static ogs_poll_t *udp_rx_poll = NULL;

ogs_queue_t *queue = NULL;
int running = 1;

ogs_sockaddr_t udp_from;

typedef enum {
    MSG_BASE = 0,
	MSG_UART,
	MSG_UDP,
    MSG_TOP,
} event_e;

typedef struct {
    event_e id;
    ogs_pkbuf_t *pkbuf;
} event_t;

static OGS_POOL(pool_uart, event_t);
static OGS_POOL(pool_udp, event_t);

static void uart_event_free(event_t *e)
{
    ogs_assert(e);
    ogs_pool_free(&pool_uart, e);
}

static event_t* uart_event_new(void)
{
    event_t *e = NULL;

    ogs_pool_alloc(&pool_uart, &e);
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));

    e->id = MSG_UART;

    return e;
}

static void udp_event_free(event_t *e)
{
    ogs_assert(e);
    ogs_pool_free(&pool_udp, e);
}

static event_t* udp_event_new(void)
{
    event_t *e = NULL;

    ogs_pool_alloc(&pool_udp, &e);
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));

    e->id = MSG_UDP;

    return e;
}

static void uart_recv_handler(short when, ogs_socket_t fd, void *data)
{
    ogs_info("uart_recv_handler enter");

    ogs_pkbuf_t *pkbuf = NULL;
    event_t *e = NULL;
    int rv = OGS_ERROR;

    pkbuf = ogs_pkbuf_alloc(NULL, MAX_PKT_LEN);
    ogs_assert(pkbuf);
    ogs_pkbuf_put(pkbuf, MAX_PKT_LEN);

	ssize_t size = read(fd, pkbuf->data, pkbuf->len);
	if (size <= 0) {
		ogs_error("uart read failed [%s]", strerror(errno));
		ogs_pkbuf_free(pkbuf);
		return;
	}
	ogs_pkbuf_trim(pkbuf, size);

	ogs_debug("==> UART RECV(size=%ld):", size);
	ogs_log_hexdump(OGS_LOG_DEBUG, pkbuf->data, pkbuf->len);

    e = uart_event_new();
    ogs_assert(e);
    e->pkbuf = pkbuf;

    rv = ogs_queue_push(queue, (void*)e);
    if (rv != OGS_OK) {
		ogs_error("queue put error");
		ogs_pkbuf_free(pkbuf);
        udp_event_free(e);
    } 
}

static void udp_recv_handler(short when, ogs_socket_t fd, void *data)
{
    ogs_info("udp_recv_handler enter");

    ogs_pkbuf_t *pkbuf = NULL;
    event_t *e = NULL;
    int rv = OGS_ERROR;
	ogs_sockaddr_t from;

    pkbuf = ogs_pkbuf_alloc(NULL, MAX_PKT_LEN);
    ogs_assert(pkbuf);
    ogs_pkbuf_put(pkbuf, MAX_PKT_LEN);

	ssize_t size = ogs_recvfrom(fd, pkbuf->data, pkbuf->len, 0, &from);
	if (size <= 0) {
		ogs_error("udp recv failed [%s]", strerror(errno));
		ogs_pkbuf_free(pkbuf);
		return;
	}
	ogs_pkbuf_trim(pkbuf, size);

	ogs_debug("==> UDP RECV(size=%ld):", size);
	ogs_log_hexdump(OGS_LOG_DEBUG, pkbuf->data, pkbuf->len);

    e = udp_event_new();
    ogs_assert(e);
    e->pkbuf = pkbuf;

    rv = ogs_queue_push(queue, (void*)e);
    if (rv != OGS_OK) {
		ogs_error("queue put error");
		ogs_pkbuf_free(pkbuf);
        uart_event_free(e);
    } 
}

static int udp_sendto(void *data, size_t len)
{
    ssize_t sent;

    ogs_assert(data);

    sent = ogs_sendto(sock->fd, data, len, 0, dst_node->addr);
    if (sent < 0 || sent != len) {
		ogs_error("ogs_sendto() failed [%s]", strerror(errno));
        return OGS_ERROR;
    }

	ogs_debug("UDP SEND: ==>");
	ogs_log_hexdump(OGS_LOG_DEBUG, data, len);

    return OGS_OK;
}

static int uart_sendto(void *data, size_t len)
{
    ssize_t sent;

    ogs_assert(data);

    sent = write(usb_fd, data, len);
    if (sent < 0 || sent != len) {
		ogs_error("uart_sendto() failed [%s]", strerror(errno));
        return OGS_ERROR;
    }

	ogs_debug("UART SEND: ==>");
	ogs_log_hexdump(OGS_LOG_DEBUG, data, len);

    return OGS_OK;
}


static void main_process(event_t *e)
{
    ogs_info("main_process enter");

	switch(e->id){
		case MSG_UART:{
			udp_sendto(e->pkbuf->data, e->pkbuf->len);
			//uart_sendto(e->pkbuf->data, e->pkbuf->len);
			ogs_pkbuf_free(e->pkbuf);
			uart_event_free(e);
			break;
		}
		case MSG_UDP:{
			uart_sendto(e->pkbuf->data, e->pkbuf->len);
			ogs_pkbuf_free(e->pkbuf);
			udp_event_free(e);
			break;
		}
		default:{
			ogs_error("unknown type");
			break;
		}
	}
}

static void main_thread(void *data)
{
    int rv = OGS_ERROR;

    ogs_info("main_thread running...");

    while(running) {
        ogs_pollset_poll(pollset, -1);
        for ( ;; ) {
            event_t *e = NULL;
            rv = ogs_queue_trypop(queue, (void**)&e);
            ogs_assert(rv != OGS_ERROR);

            if (rv == OGS_DONE)
                goto done;

            if (rv == OGS_RETRY)
                break;

            ogs_assert(e);
			main_process(e);
        }
    }
	
done:
	return;
}


static int check_signal(int signum)
{
    switch (signum) {
    case SIGTERM:
    case SIGINT:
        ogs_info("%s received", 
                signum == SIGTERM ? "SIGTERM" : "SIGINT");

        return 1;
    case SIGHUP:
        ogs_info("SIGHUP received");
        ogs_log_cycle();

        break;
    case SIGWINCH:
        ogs_info("Signal-NUM[%d] received (%s)",
                signum, ogs_signal_description_get(signum));
        break;
    default:
        ogs_error("Signal-NUM[%d] received (%s)",
                signum, ogs_signal_description_get(signum));
        break;
            
    }
    return 0;
}

static void terminate(void)
{	
    running = 0;
	ogs_pollset_notify(pollset);
	ogs_queue_term(queue);
	
	ogs_pollset_remove(uart_rx_poll);
	ogs_pollset_remove(udp_rx_poll);
	close(usb_fd);
	ogs_pollset_destroy(pollset);

	ogs_sock_destroy(sock);
	ogs_socknode_free(node);
	ogs_socknode_free(dst_node);

	ogs_queue_destroy(queue);

	ogs_thread_destroy(thread);

    ogs_pkbuf_default_destroy();
	ogs_pool_final(&pool_uart);
    ogs_pool_final(&pool_udp);
	ogs_core_terminate();

}

static void uart_initial(void)
{
    // 打开串口
    usb_fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY | O_NDELAY);
    if (usb_fd == -1) {
        ogs_error("open %s error", SERIAL_PORT);
        return;
    }
	fcntl(usb_fd, F_SETFL, 0);
    // 配置串口
    struct termios options;
    tcgetattr(usb_fd, &options);
    cfsetispeed(&options, B115200);  // 输入波特率为115200
    cfsetospeed(&options, B115200);  // 输出波特率为115200
    options.c_cflag &= ~CSIZE;      // 8位数据位
    options.c_cflag |= CS8;
    options.c_cflag &= ~PARENB;     // 无校验位
    options.c_cflag &= ~CSTOPB;     // 1位停止位

	//而为了保持原始输入模式，我们需要控制的是输入标志和本地标志，将控制标志设置为屏蔽各种控制字，然后输入标志设置为屏蔽各种转义，最后控制字段如下
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); /*Input LOCAL*/
	options.c_oflag &= ~OPOST; /*Output*/
	options.c_iflag &= ~(IXON | IXOFF | IXANY |BRKINT | ICRNL | ISTRIP );

    tcsetattr(usb_fd, TCSANOW, &options);

	uart_rx_poll = ogs_pollset_add(pollset, OGS_POLLIN, usb_fd, uart_recv_handler, NULL);
}

static void udp_initial(void)
{
    int ret = OGS_ERROR;
    ogs_sockaddr_t *addr = NULL, *dst_addr = NULL;

    ret = ogs_addaddrinfo(&addr, AF_UNSPEC, udp_address.ip, udp_address.port, 0);
    ogs_assert(ret == OGS_OK);
    node = ogs_socknode_new(addr);
    ogs_assert(node);
	sock = ogs_udp_server(node);
	ogs_assert(sock);
	udp_rx_poll = ogs_pollset_add(pollset, OGS_POLLIN, sock->fd, udp_recv_handler, NULL);

    ret = ogs_addaddrinfo(&dst_addr, AF_UNSPEC, dst_udp_address.ip, dst_udp_address.port, 0);
    ogs_assert(ret == OGS_OK);
    dst_node = ogs_socknode_new(dst_addr);
    ogs_assert(dst_node);
}


int main(int argc, const char *const argv[])
{
    ogs_pkbuf_config_t config;
    /*ogs_getopt_t options;
    const char *argv_out[argc];
    int opt;

    memset(&optarg, 0, sizeof(optarg));

    ogs_getopt_init(&options, (char**)argv);
    while ((opt = ogs_getopt(&options, "i:p:d:")) != -1) {
        switch (opt) {
        case 'i':
            udp_address.ip = ogs_strdup(options.optarg);
			printf("udp ip:%s\n", udp_address.ip);
            return OGS_OK;
        case 'p':
            udp_address.port = atoi(options.optarg);
			printf("udp port:%d\n", udp_address.port);
            return OGS_OK;
        case 'd':
			memcpy(SERIAL_PORT, options.optarg, sizeof(SERIAL_PORT));
            printf("usb edvice:%s\n", SERIAL_PORT);
            break;
        default:
            fprintf(stderr, "%s: should not be reached\n", OGS_FUNC);
            return OGS_ERROR;
        }		
	}*/

	ogs_core_initialize();
    ogs_pkbuf_default_init(&config);
    ogs_pkbuf_default_create(&config);
    ogs_setup_signal_thread();

    ogs_pool_init(&pool_uart, QUEUE_SIZE);
    ogs_pool_init(&pool_udp, QUEUE_SIZE);
	queue = ogs_queue_create(QUEUE_SIZE*2);
	pollset = ogs_pollset_create(4);

	uart_initial();
	udp_initial();

    thread = ogs_thread_create(main_thread, NULL);
    if (!thread) return OGS_ERROR;

    atexit(terminate);
    ogs_signal_thread(check_signal);

    ogs_info("terminating...");

    return 0;
}
