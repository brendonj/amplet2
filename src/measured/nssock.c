#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unbound.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "nssock.h"
#include "ampresolv.h"
#include "debug.h"



/*
 * Receive all the queries from the local socket and wait for them to resolve
 * before sending them back to the test process.
 */
static void *amp_resolver_worker_thread(void *thread_data) {
    struct amp_resolve_info *data = (struct amp_resolve_info *)thread_data;
    struct addrinfo *addrlist = NULL, *item;
    char name[MAX_DNS_NAME_LEN];
    struct amp_resolve_query info;
    uint8_t more;
    uint8_t namelen;
    int bytes;
    pthread_mutex_t addrlist_lock;
    int remaining = 0;

    Log(LOG_DEBUG, "Starting new name resolution thread");

    pthread_mutex_init(&addrlist_lock, NULL);

    /* everything we read should be a name to lookup */
    while ( 1 ) {

        /* read name length, address count, family etc */
        if ( recv(data->fd, &info, sizeof(info), 0) <= 0 ) {
            Log(LOG_WARNING, "Error reading name info, aborting");
            break;
        }

        if ( (bytes = recv(data->fd, name, info.namelen, 0)) <= 0 ) {
            Log(LOG_WARNING, "Error reading name, aborting");
            break;
        }

        Log(LOG_DEBUG, "Read %d bytes for name '%s'", bytes, name);

        /* add it to the list of names to resolve and go back for more */
        amp_resolve_add(data->ctx, &addrlist, &addrlist_lock, name,
                info.family, info.count, &remaining);

        if ( !info.more ) {
            break;
        }
    }

    Log(LOG_DEBUG, "Got all requests, waiting for responses");

    /* when the remote end has finished sending names, wait for resolution */
    amp_resolve_wait(data->ctx, &addrlist_lock, &remaining);

    /* once we have all the responses then we don't need the addrlist lock */
    pthread_mutex_lock(&addrlist_lock);

    Log(LOG_DEBUG, "Got all responses, sending them back");

    /* send back all the results of name resolution */
    for ( item = addrlist; item != NULL; item = item->ai_next) {
        if ( send(data->fd, item, sizeof(*item), 0) < 0 ) {
            Log(LOG_WARNING, "Failed to send resolved address info: %s",
                    strerror(errno));
            goto end;
        }

        if ( send(data->fd, item->ai_addr, item->ai_addrlen, 0) < 0 ) {
            Log(LOG_WARNING, "Failed to send resolved address: %s",
                    strerror(errno));
            goto end;
        }

        namelen = strlen(item->ai_canonname) + 1;
        assert(namelen > 1);
        if ( send(data->fd, &namelen, sizeof(namelen), 0) < 0 ) {
            Log(LOG_WARNING, "Failed to send resolved canonical name: %s",
                    strerror(errno));
            goto end;
        }

        assert(item->ai_canonname);
        if ( send(data->fd, item->ai_canonname, namelen, 0) < 0 ) {
            Log(LOG_WARNING, "Failed to send resolved canonical name: %s",
                    strerror(errno));
            goto end;
        }

        more = (item->ai_next) ? 1 : 0;
        if ( send(data->fd, &more, sizeof(uint8_t), 0) < 0 ) {
            Log(LOG_WARNING, "Failed to send more flag: %s", strerror(errno));
            goto end;
        }
    }

    Log(LOG_DEBUG, "Name resolution thread completed, exiting");

end:
    close(data->fd);
    amp_resolve_freeaddr(addrlist);
    pthread_mutex_unlock(&addrlist_lock);
    pthread_mutex_destroy(&addrlist_lock);
    free(data);

    pthread_exit(NULL);
}



/*
 * Set up the unbound context that we will use to query (even if no nameservers
 * are specified, it will be able to cache results).
 */
struct ub_ctx *amp_resolver_context_init(char *servers[], int nscount,
        char *sourcev4, char *sourcev6) {

    struct ub_ctx *ctx;
    int i;

    Log(LOG_DEBUG, "Initialising nameserver context with %d servers", nscount);

    /* create the context */
    if ( (ctx = ub_ctx_create()) == NULL ) {
        Log(LOG_WARNING, "Could not create unbound context\n");
        return NULL;
    }

    /* use threads for asynchronous resolving */
    if ( ub_ctx_async(ctx, 1) < 0 ) {
        Log(LOG_WARNING, "error enabling threading in resolver\n");
        return NULL;
    }

    //XXX call ub_ctx_resolvconf if no nameservers set? or is that default?

    /* set the nameservers that we should query, if they are specified */
    for ( i = 0; i < nscount; i++ ) {
        Log(LOG_DEBUG, "Adding %s as nameserver", servers[i]);
        if ( ub_ctx_set_fwd(ctx, servers[i]) < 0 ) {
            Log(LOG_WARNING, "error setting forward address to %s\n",
                    servers[i]);
        }
    }

    /* use only the given outgoing interfaces if they have been specified */
    if ( sourcev4 ) {
        ub_ctx_set_option(ctx, "outgoing-interface", sourcev4);
    }

    if ( sourcev6 ) {
        ub_ctx_set_option(ctx, "outgoing-interface", sourcev6);
    }

    return ctx;
}



/*
 * Delete the unbound context.
 */
void amp_resolver_context_delete(struct ub_ctx *ctx) {
    assert(ctx);
    ub_ctx_delete(ctx);
}



/*
 * Create the local unix socket that will listen for DNS requests from test
 * processes.
 */
int initialise_resolver_socket(char *path) {
    int sock;
    struct sockaddr_un addr;

    Log(LOG_DEBUG, "Creating local socket at '%s' for name resolution", path);

    /*
     * We shouldn't be able to get to here if there is already an amp
     * process running with our name, so clearing out the socket should
     * be a safe thing to do.
     */
    if ( access(path, F_OK) == 0 ) {
        Log(LOG_DEBUG, "Socket '%s' exists, removing", path);
        if ( unlink(path) < 0 ) {
            Log(LOG_WARNING, "Failed to remove old socket '%s': %s", path,
                    strerror(errno));
            return -1;
        }
    }

    /* start listening on a unix socket */
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, UNIX_PATH_MAX, "%s", path);

    if ( (sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ) {
        Log(LOG_WARNING, "Failed to open local socket for name resolution: %s",
                strerror(errno));
        return -1;
    }

    if ( bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 ) {
        Log(LOG_WARNING, "Failed to bind local socket for name resolution: %s",
                strerror(errno));
        return -1;
    }

    /*
     * TODO what sort of backlog is appropriate here? How many tests are
     * starting at the same time?
     */
    if ( listen(sock, 16) < 0 ) {
        Log(LOG_WARNING,
                "Failed to listen on local socket for name resolution: %s",
                strerror(errno));
        return -1;
    }

    return sock;
}



/*
 * Accept a new connection on the local name resolution socket and spawn
 * a new thread to deal with the queries from the test process.
 */
void resolver_socket_event_callback(struct wand_fdcb_t *handle,
        __attribute__((unused))enum wand_eventtype_t ev) {

    int fd;
    pthread_t thread;
    struct amp_resolve_info *data;

    Log(LOG_DEBUG, "Accepting for new resolver connection");

    if ( (fd = accept(handle->fd, NULL, NULL)) < 0 ) {
        Log(LOG_WARNING, "Failed to accept for name resolution: %s",
                strerror(errno));
        return;
    }

    Log(LOG_DEBUG, "Accepted new resolver connection on fd %d", fd);

    data = calloc(1, sizeof(struct amp_resolve_info));
    data->ctx = handle->data;
    data->fd = fd;

    /* create the thread and detach, we don't need to look after it */
    pthread_create(&thread, NULL, amp_resolver_worker_thread, data);
    pthread_detach(thread);
}