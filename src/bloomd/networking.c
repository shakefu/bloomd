#include "networking.h"
#define EV_STANDALONE 1
#define EV_COMPAT3 0
#define EV_MULTIPLICITY 0
#ifdef __linux__
#define EV_USE_EPOLL 1
#endif
#ifdef __APPLE__
#define EV_USE_KQUEUE 1
#endif
#include "ev.c"
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netinet/in.h>

/**
 * Default listen backlog size for
 * our TCP listener.
 */
#define BACKLOG_SIZE 64

/**
 * How big should the initial conns
 * array be in terms of slots. For most
 * cases 1024 is more than we will need,
 * and will fit nicely in 1 page on 32bits,
 * and 2 pages on 64bits.
 */
#define INIT_CONN_LIST_SIZE 1024


/**
 * Stores the thread specific user data.
 */
typedef struct {
    ev_io *watcher;
    int ready_events;
} worker_ev_userdata;


/**
 * Stores the connection specific data.
 * We initialize one of these per connection
 */
typedef struct {
    ev_io client;
    uint32_t buf_size;
    char *buffer;
} conn_info;


/**
 * Defines a structure that is
 * used to store the state of the networking
 * stack.
 */
struct bloom_networking {
    bloom_config *config;
    pthread_mutex_t leader_lock; // Serializes the leaders
    int tcp_listener_fd;
    int udp_listener_fd;
    ev_io tcp_client;
    ev_io udp_client;
    volatile int should_run;  // Should the workers continue to run
    volatile int num_threads; // Number of threads in the threads list
    pthread_t *threads;       // Array of thread references
    int conn_list_size;       // Maximum size of conns list
    conn_info **conns;        // An array of pointers to conn_info objects
};


// Static typedefs
static void prepare_event(ev_io *watcher, int revents);
static void invoke_event_handler(worker_ev_userdata* data);


/**
 * Initializes the TCP listener
 * @arg netconf The network configuration
 * @return 0 on success.
 */
static int setup_tcp_listener(bloom_networking *netconf) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = PF_INET;
    addr.sin_port = htons(netconf->config->tcp_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Make the socket, bind and listen
    netconf->tcp_listener_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (bind(netconf->tcp_listener_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        syslog(LOG_ERR, "Failed to bind on TCP socket! Err: %s", strerror(errno));
        close(netconf->tcp_listener_fd);
        return 1;
    }
    if (listen(netconf->tcp_listener_fd, BACKLOG_SIZE) != 0) {
        syslog(LOG_ERR, "Failed to listen on TCP socket! Err: %s", strerror(errno));
        close(netconf->tcp_listener_fd);
        return 1;
    }

    // Create the libev objects
    ev_io_init(&netconf->tcp_client, prepare_event,
                netconf->tcp_listener_fd, EV_READ);
    ev_io_start(&netconf->tcp_client);
    return 0;
}

/**
 * Initializes the UDP Listener.
 * @arg netconf The network configuration
 * @return 0 on success.
 */
static int setup_udp_listener(bloom_networking *netconf) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = PF_INET;
    addr.sin_port = htons(netconf->config->udp_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Make the socket, bind and listen
    netconf->udp_listener_fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (bind(netconf->udp_listener_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        syslog(LOG_ERR, "Failed to bind on UDP socket! Err: %s", strerror(errno));
        close(netconf->udp_listener_fd);
        return 1;
    }

    // Create the libev objects
    ev_io_init(&netconf->udp_client, prepare_event,
                netconf->udp_listener_fd, EV_READ);
    ev_io_start(&netconf->udp_client);
    return 0;
}

/**
 * Initializes the networking interfaces
 * @arg config Takes the bloom server configuration
 * @arg netconf Output. The configuration for the networking stack.
 */
int init_networking(bloom_config *config, bloom_networking **netconf_out) {
    // Make the netconf structure
    bloom_networking *netconf = calloc(1, sizeof(struct bloom_networking));

    // Initialize
    pthread_mutex_init(&netconf->leader_lock, NULL);
    netconf->config = config;
    netconf->should_run = 1;
    netconf->num_threads = 0;
    netconf->threads = calloc(config->worker_threads, sizeof(pthread_t));
    netconf->conn_list_size = INIT_CONN_LIST_SIZE;
    netconf->conns = calloc(INIT_CONN_LIST_SIZE, sizeof(conn_info*));

    // Setup the TCP listener
    int res = setup_tcp_listener(netconf);
    if (res != 0) {
        free(netconf);
        return 1;
    }

    // Setup the UDP listener
    res = setup_udp_listener(netconf);
    if (res != 0) {
        ev_io_stop(&netconf->tcp_client);
        close(netconf->tcp_listener_fd);
        free(netconf);
        return 1;
    }

    // Success!
    *netconf_out = netconf;
    return 0;
}


/**
 * Called when an event is ready to be processed by libev.
 * We need to do _very_ little work here. Basically just
 * setup the userdata to process the event and return.
 * This is so we can release the leader lock and let another
 * thread take over.
 */
static void prepare_event(ev_io *watcher, int revents) {
    // Get the user data
    worker_ev_userdata *data = ev_userdata();

    // Set everything
    data->watcher = watcher;
    data->ready_events = revents;

    // Stop listening for now
    ev_io_stop(watcher);
}


/**
 * Reads the thread specific userdata to figure out what
 * we need to handle. Things that purely effect the network
 * stack should be handled here, but otherwise we should defer
 * to the connection handlers.
 */
static void invoke_event_handler(worker_ev_userdata* data) {

}


/**
 * Entry point for threads to join the networking
 * stack. This method blocks indefinitely until the
 * network stack is shutdown.
 * @arg netconf The configuration for the networking stack.
 */
void start_networking_worker(bloom_networking *netconf) {
    // Allocate our user data
    worker_ev_userdata data;
    int registered = 0;

    // Run forever until we are told to halt
    while (netconf->should_run) {
        // Become the leader
        pthread_mutex_lock(&netconf->leader_lock);

        // Register if we need to
        if (!registered) {
            netconf->threads[netconf->num_threads] = pthread_self();
            netconf->num_threads++;
            registered = 1;
        }

        // Check again if we should run
        if (netconf->should_run) {
            pthread_mutex_unlock(&netconf->leader_lock);
            break;
        }

        // Set the user data to be for this thread
        ev_set_userdata(&data);

        // Run one iteration of the event loop
        ev_run(EVRUN_ONCE);

        // Release the leader lock
        pthread_mutex_unlock(&netconf->leader_lock);

        // Process the event
        invoke_event_handler(&data);
    }
    return;
}

/**
 * Shuts down all the connections
 * and listeners and prepares to exit.
 * @arg netconf The config for the networking stack.
 */
int shutdown_networking(bloom_networking *netconf) {
    // Instruct the threads to shutdown
    netconf->should_run = 0;

    // Break the EV loop
    ev_break(EVBREAK_ALL);

    // Wait for the threads to return
    pthread_t thread;
    for (int i=0; i < netconf->num_threads; i++) {
        thread = netconf->threads[i];
        if (thread != NULL) pthread_join(thread, NULL);
    }

    // Stop listening for new connections
    ev_io_stop(&netconf->tcp_client);
    close(netconf->tcp_listener_fd);
    ev_io_stop(&netconf->udp_client);
    close(netconf->udp_listener_fd);

    // Close all the client connections
    conn_info *conn;
    for (int i=0; i < netconf->conn_list_size; i++) {
        // Check if the connection is non-null
        conn = netconf->conns[i];
        if (conn == NULL) continue;

        // Stop listening in libev and close the socket
        ev_io_stop(&conn->client);
        close(conn->client.fd);

        // Free all the buffers
        free(conn->buffer);
        free(conn);
    }

    // Free the netconf
    free(netconf->threads);
    free(netconf->conns);
    free(netconf);
    return 0;
}


