#include <getopt.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "udpstream.h"
#include "serverlib.h" //XXX this needs a better name
#include "udpstream.pb-c.h"



static void report_results(struct timeval *start_time, struct addrinfo *dest,
        struct opt_t *options, struct timeval *in_times,
        Amplet2__Udpstream__Item *server_report) {

    Amplet2__Udpstream__Report msg = AMPLET2__UDPSTREAM__REPORT__INIT;
    Amplet2__Udpstream__Header header = AMPLET2__UDPSTREAM__HEADER__INIT;
    Amplet2__Udpstream__Item **reports = NULL;
    unsigned int i = 0;
    void *buffer;
    int len;

    /* populate the header with all the test options */
    header.has_family = 1;
    header.family = dest->ai_family;
    header.has_packet_size = 1;
    header.packet_size = options->packet_size;
    header.has_packet_spacing = 1;
    header.packet_spacing = options->packet_spacing;
    header.has_packet_count = 1;
    header.packet_count = options->packet_count;
    header.has_percentile_count = 1;
    header.percentile_count = options->percentile_count;
    header.name = address_to_name(dest);
    header.has_address = copy_address_to_protobuf(&header.address, dest);

    /* only report the results that are available */
    if ( in_times && server_report ) {
        msg.n_reports = 2;
    } else if ( in_times || server_report ) {
        msg.n_reports = 1;
    } else {
        assert(0);
    }

    reports = calloc(msg.n_reports, sizeof(Amplet2__Udpstream__Item*));

    if ( in_times ) {
        reports[i++] = report_stream(in_times, options);
    }

    if ( server_report ) {
        reports[i++] = server_report;
    }

    /* populate the top level report object with the header and reports */
    msg.header = &header;
    msg.reports = reports;

    /* pack all the results into a buffer for transmitting */
    len = amplet2__udpstream__report__get_packed_size(&msg);
    buffer = malloc(len);
    amplet2__udpstream__report__pack(&msg, buffer);

    /* send the packed report object */
    report(AMP_TEST_UDPSTREAM, (uint64_t)start_time->tv_sec, (void*)buffer,len);

    /* free up all the memory we had to allocate to report items */
    for ( i = 0; i < msg.n_reports; i++ ) {
        free(reports[i]);
    }

    free(reports);
    free(buffer);
}



/*
 * TODO could this be a library function too, with a function pointer?
 */
static int run_test(struct addrinfo *server, struct opt_t *options,
        struct temp_sockopt_t_xxx *socket_options) {
    int control_socket, test_socket;
    //struct temp_sockopt_t_xxx optxxx;
    struct sockaddr_storage ss;
    socklen_t socklen = sizeof(ss);
    struct timeval *in_times = NULL;
    struct test_request_t *schedule = NULL, *current;
    ProtobufCBinaryData data;
    Amplet2__Udpstream__Item *results = NULL;
    struct timeval start_time;


    printf("run test\n");
    socket_options->cport = options->cport;//XXX
    socket_options->tport = options->tport;//XXX
    socket_options->packet_size = options->packet_size;//XXX
    socket_options->packet_count = options->packet_count;//XXX
    socket_options->packet_spacing = options->packet_spacing;//XXX
    socket_options->percentile_count = options->percentile_count;//XXX

    socket_options->socktype = SOCK_STREAM;
    socket_options->protocol = IPPROTO_TCP;

    /* create our test socket so it is ready early on */
    if ( (test_socket=socket(server->ai_family, SOCK_DGRAM, IPPROTO_UDP)) < 0 ){
        Log(LOG_WARNING, "Failed to create control socket:%s", strerror(errno));
        return -1;
    }

    /* connect to the control socket on the server */
    control_socket = connect_to_server(server, socket_options,
            options->cport);//XXX socket_options?
    printf("control = %d\n", control_socket);

    gettimeofday(&start_time, NULL);

    /* send hello */
    if ( send_control_hello(control_socket, socket_options) < 0 ) {
        Log(LOG_WARNING, "Failed to send HELLO packet, aborting");
        close(control_socket);
        return -1;
    }

    /* run the test schedule */
    switch ( options->direction ) {
        case CLIENT_TO_SERVER:
            printf("CLIENT TO SERVER SCHEDULE\n");
            schedule = calloc(1, sizeof(struct test_request_t));
            schedule->direction = UDPSTREAM_TO_SERVER;
            break;

        case SERVER_TO_CLIENT:
            printf("SERVER TO CLIENT SCHEDULE\n");
            schedule = calloc(1, sizeof(struct test_request_t));
            schedule->direction = UDPSTREAM_TO_CLIENT;
            break;

        case SERVER_THEN_CLIENT:
            printf("SERVER THEN CLIENT SCHEDULE\n");
            schedule = calloc(2, sizeof(struct test_request_t));
            schedule[0].direction = UDPSTREAM_TO_CLIENT;
            schedule[0].next = &schedule[1];
            schedule[1].direction = UDPSTREAM_TO_SERVER;
            break;

        case CLIENT_THEN_SERVER:
            printf("CLIENT THEN SERVER SCHEDULE\n");
            schedule = calloc(2, sizeof(struct test_request_t));
            schedule[0].direction = UDPSTREAM_TO_SERVER;
            schedule[0].next = &schedule[1];
            schedule[1].direction = UDPSTREAM_TO_CLIENT;
            break;

        default:
            /* XXX */
            printf("BROKEN SCHEDULE\n");
            break;
    };

    for ( current = schedule; current != NULL; current = current->next ) {
        printf("SCHEDULE ITEM START\n");
        switch ( current->direction ) {
            case UDPSTREAM_TO_SERVER:
                send_control_receive(control_socket, options->packet_count);

                if ( read_control_ready(control_socket, socket_options) < 0 ) {
                    Log(LOG_WARNING, "Failed to read READY packet, aborting");
                    close(control_socket);
                    return -1;
                }
                //XXX
                printf("test port = %d\n", socket_options->tport);
                ((struct sockaddr_in *)server->ai_addr)->sin_port =
                    ntohs(socket_options->tport);

                send_udp_stream(test_socket, server, options);

                /* wait for the results from the stream we just sent */
                if ( read_control_result(control_socket, &data) < 0 ) {
                    Log(LOG_WARNING, "Failed to read RESULT packet, aborting");
                    close(control_socket);
                    return -1;
                }
                results = amplet2__udpstream__item__unpack(NULL, data.len,
                        data.data);
                break;

            case UDPSTREAM_TO_CLIENT:
                in_times = calloc(options->packet_count, sizeof(struct timeval));
                /* bind test socket to same address as the control socket */
                getsockname(control_socket, (struct sockaddr *)&ss, &socklen);
                bind(test_socket, (struct sockaddr *)&ss, socklen);
                /* get the local port number so we can tell the remote host */
                getsockname(test_socket, (struct sockaddr *)&ss, &socklen);
                socket_options->tport = ntohs(((struct sockaddr_in *)&ss)->sin_port);

                send_control_send(control_socket,
                        ntohs(((struct sockaddr_in *)&ss)->sin_port));

                /* wait for the data stream from the server */
                receive_udp_stream(test_socket, options->packet_count, in_times);

                break;
        };
    }

    /* report results */
    report_results(&start_time, server, options, in_times, results);

    return 0;
}



/*
 *
 */
int run_udpstream_client(int argc, char *argv[], int count,
        struct addrinfo **dests) {

    int opt;
    struct opt_t test_options;
    struct temp_sockopt_t_xxx socket_options;
    struct info_t *info;
    struct addrinfo *sourcev4, *sourcev6;
    char *device;
    char *client;
    amp_test_meta_t meta;
    extern struct option long_options[];

    Log(LOG_DEBUG, "Starting udpstream test");

    /* set some sensible defaults */
    //XXX set better inter packet delay, using MIN as a floor?
    test_options.packet_spacing = MIN_INTER_PACKET_DELAY;
    test_options.packet_size = DEFAULT_UDPSTREAM_PACKET_LENGTH;
    test_options.packet_count = DEFAULT_UDPSTREAM_PACKET_COUNT;
    test_options.percentile_count = DEFAULT_UDPSTREAM_PERCENTILE_COUNT;
    test_options.cport = DEFAULT_CONTROL_PORT;
    test_options.tport = DEFAULT_TEST_PORT;
    test_options.perturbate = 0;
    test_options.direction = CLIENT_THEN_SERVER;
    socket_options.sourcev4 = NULL;
    socket_options.sourcev6 = NULL;
    socket_options.device = NULL;
    client = NULL;

    /* TODO udp port */
    while ( (opt = getopt_long(argc, argv, "hvI:Z:p:rz:c:d:n:4:6:",
                    long_options, NULL)) != -1 ) {
	switch ( opt ) {
            case '4':
                socket_options.sourcev4 = get_numeric_address(optarg, NULL);
                break;
            case '6':
                socket_options.sourcev6 = get_numeric_address(optarg, NULL);
                break;
            case 'I': socket_options.device = optarg; break;
            case 'c': client = optarg; break;
            case 'Z': test_options.packet_spacing = atoi(optarg); break;
	    case 'p': test_options.perturbate = atoi(optarg); break;
	    case 'z': test_options.packet_size = atoi(optarg); break;
	    case 'n': test_options.packet_count = atoi(optarg); break;
            case 'd': test_options.direction = atoi(optarg); break;
            case 'v': version(argv[0]); exit(0);
	    case 'h':
	    default: usage(argv[0]); exit(0);
	};
    }

    /*
     * Don't do anything if the test provides a target through the dests
     * parameter as well as using -c. They expect the server to behave slightly
     * differently, so we can't tell which the user wants.
     */
    if ( dests && client ) {
        Log(LOG_WARNING, "Option -c not valid when target address already set");
        exit(1);
    }

    //XXX move into common/serverlib.c? use for throughput too
    /* if the -c option is set then get the address into the dests parameter */
    if ( client ) {
        int res;
        struct addrinfo hints;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC; // XXX depends on if source is given?
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        dests = (struct addrinfo **)malloc(sizeof(struct addrinfo *));
        if ( (res = getaddrinfo(client, NULL, &hints, &dests[0])) < 0 ) {
            Log(LOG_WARNING, "Failed to resolve '%s': %s", client,
                    gai_strerror(res));
            exit(1);
        }

        /* just take the first address we find */
        count = 1;

        /* set the canonical name to be the address so we can print it later */
        dests[0]->ai_canonname = strdup(client);
    }

    /*
     * Make sure we got a destination, either through the dests parameter
     * or the -c command line argument
     */
    if ( count < 1 || dests == NULL || dests[0] == NULL ) {
        Log(LOG_WARNING, "No destination specified for throughput test");
        exit(1);
    }

    /* make sure that the packet size is big enough for our data */
    if ( test_options.packet_size < MINIMUM_UDPSTREAM_PACKET_LENGTH ) {
	Log(LOG_WARNING, "Packet size %d below minimum size, raising to %d",
		test_options.packet_size, MINIMUM_UDPSTREAM_PACKET_LENGTH);
	test_options.packet_size = MINIMUM_UDPSTREAM_PACKET_LENGTH;
    }

    /* delay the start by a random amount of perturbate is set */
    if ( test_options.perturbate ) {
	int delay;
	delay = test_options.perturbate * 1000 * (random()/(RAND_MAX+1.0));
	Log(LOG_DEBUG, "Perturbate set to %dms, waiting %dus",
		test_options.perturbate, delay);
	usleep(delay);
    }

    /*
     * Only start the remote server if we expect it to be running as part
     * of amplet2/measured, otherwise it should be already running standalone.
     */
    if ( client == NULL ) {
        int remote_port;
        if ( (remote_port = start_remote_server(AMP_TEST_UDPSTREAM,
                        dests[0], &meta)) == 0 ) {
            Log(LOG_WARNING, "Failed to start remote server, aborting test");
            exit(1);
        }

        Log(LOG_DEBUG, "Got port %d from remote server", remote_port);
        test_options.cport = remote_port;
    }

    run_test(dests[0], &test_options, &socket_options);

    if ( client != NULL ) {
        freeaddrinfo(dests[0]);
        free(dests);
    }

    return 0;
}



/*
 *
 */
static void print_item(Amplet2__Udpstream__Item *item, uint32_t packet_count) {
    uint32_t i;

    assert(item);

    if ( item->direction ==
            AMPLET2__UDPSTREAM__ITEM__DIRECTION__SERVER_TO_CLIENT ) {
        printf("  * server -> client:");
    } else if ( item->direction ==
            AMPLET2__UDPSTREAM__ITEM__DIRECTION__CLIENT_TO_SERVER ) {
        printf("  * client -> server:");
    } else {
        printf("TODO set direction\n");
        //return;
    }

    printf("%d packets transmitted, %d received, %.02f%% packet loss\n",
            packet_count, item->packets_received,
            100 - ((double)item->packets_received / (double)packet_count*100));

    printf("delay variation min/median/max = %d/%d/%d\n",
            item->minimum, item->median, item->maximum);

    printf("percentiles:");
    for ( i = 0; i < item->n_percentiles; i++ ) {
        printf(" %d:%d", (i+1) * 10, item->percentiles[i]);
    }
    printf("\n");
}



void print_udpstream(void *data, uint32_t len) {
    Amplet2__Udpstream__Report *msg;
    unsigned int i;
    char addrstr[INET6_ADDRSTRLEN];

    assert(data != NULL);

    /* unpack all the data */
    msg = amplet2__udpstream__report__unpack(NULL, len, data);

    assert(msg);
    assert(msg->header);

    /* print global configuration options */
    printf("\n");
    inet_ntop(msg->header->family, msg->header->address.data, addrstr,
            INET6_ADDRSTRLEN);
    printf("AMP udpstream test to %s (%s)\n", msg->header->name, addrstr);
    printf("packet count:%" PRIu32 " size:%" PRIu32 " spacing:%" PRIu32 "\n",
            msg->header->packet_count, msg->header->packet_size,
            msg->header->packet_spacing);

    for ( i=0; i < msg->n_reports; i++ ) {
        print_item(msg->reports[i], msg->header->packet_count);
    }

    amplet2__udpstream__report__free_unpacked(msg, NULL);
}