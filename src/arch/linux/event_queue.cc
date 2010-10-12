
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <errno.h>
#include <strings.h>
#include <new>
#include <algorithm>
#include <string>
#include <sstream>
#include "config/args.hpp"
#include "config/alloc.hpp"
#include "utils2.hpp"
#include "arch/linux/io.hpp"
#include "arch/linux/event_queue.hpp"
#include "arch/linux/thread_pool.hpp"

// TODO: report event queue statistics.

linux_event_queue_t::linux_event_queue_t(linux_queue_parent_t *parent)
    : parent(parent),
      events_per_loop(0), pm_events_per_loop("events_per_loop", &events_per_loop, &perfmon_combiner_average)
{
    // Create a poll fd
    
    epoll_fd = epoll_create1(0);
    check("Could not create epoll fd", epoll_fd == -1);
}

void linux_event_queue_t::run() {
    
    int res;
    
    // Now, start the loop
    while (!parent->should_shut_down()) {
    
        // Grab the events from the kernel!
        res = epoll_wait(epoll_fd, events, MAX_IO_EVENT_PROCESSING_BATCH_SIZE, -1);
        
        // epoll_wait might return with EINTR in some cases (in
        // particular under GDB), we just need to retry.
        if(res == -1 && errno == EINTR) {
            continue;
        }
        check("Waiting for epoll events failed", res == -1);

        // nevents might be used by forget_resource during the loop
        nevents = res;

        // TODO: instead of processing the events immediately, we
        // might want to queue them up and then process the queue in
        // bursts. This might reduce response time but increase
        // overall throughput because it will minimize cache faults
        // associated with instructions as well as data structures
        // (see section 7 [CPU scheduling] in B-tree Indexes and CPU
        // Caches by Goetz Graege and Pre-Ake Larson).

        for (int i = 0; i < nevents; i++) {
            if (events[i].data.ptr == NULL) {
                // The event was queued for a resource that's
                // been destroyed, so forget_resource is kindly
                // notifying us to skip it.
                continue;
            } else {
                linux_epoll_callback_t *cb = (linux_epoll_callback_t*)events[i].data.ptr;
                cb->on_epoll(events[i].events);
            }
        }

        events_per_loop = nevents;
        nevents = 0;
        
        parent->pump();
    }
}

linux_event_queue_t::~linux_event_queue_t()
{
    int res;
    
    res = close(epoll_fd);
    check("Could not close epoll_fd", res != 0);
}

void linux_event_queue_t::watch_resource(fd_t resource, int watch_mode, linux_epoll_callback_t *cb) {

    assert(cb);
    epoll_event event;
    
    event.events = watch_mode;
    event.data.ptr = (void*)cb;
    
    int res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, resource, &event);
    check("Could not pass socket to worker", res != 0);
}

void linux_event_queue_t::forget_resource(fd_t resource, linux_epoll_callback_t *cb) {

    assert(cb);
    
    epoll_event event;
    
    event.events = EPOLLIN;
    event.data.ptr = NULL;
    
    int res = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, resource, &event);
    check("Couldn't remove socket from watching", res != 0);

    // Go through the queue of messages in the current poll cycle and
    // clean out the ones that are referencing the resource we're
    // being asked to forget.
    for (int i = 0; i < nevents; i++) {
        if (events[i].data.ptr == (void*)cb) {
            events[i].data.ptr = NULL;
        }
    }
}
