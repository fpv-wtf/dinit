#ifndef DINIT_CONTROL_H
#define DINIT_CONTROL_H

#include <list>
#include <vector>
#include <unordered_map>
#include <map>
#include <limits>
#include <cstddef>

#include <unistd.h>

#include "dasynq.h"

#include "dinit.h"
#include "dinit-log.h"
#include "control-cmds.h"
#include "service-listener.h"
#include "cpbuffer.h"

// Control connection for dinit

class control_conn_t;
class control_conn_watcher;

// forward-declaration of callback:
static dasynq::rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);

// Pointer to the control connection that is listening for rollback completion
extern control_conn_t * rollback_handler_conn;

extern int active_control_conns;

// "packet" format:
// (1 byte) packet type
// (N bytes) additional data (service name, etc)
//   for LOADSERVICE/FINDSERVICE:
//      (2 bytes) service name length
//      (M bytes) service name (without nul terminator)

// Information packet:
// (1 byte) packet type, >= 100
// (1 byte) packet length (including all fields)
//       N bytes: packet data (N = (length - 2))

class service_set;
class service_record;

class control_conn_watcher : public eventloop_t::bidi_fd_watcher_impl<control_conn_watcher>
{
    inline rearm receive_event(eventloop_t &loop, int fd, int flags) noexcept;

    eventloop_t * event_loop;

    public:
    control_conn_watcher(eventloop_t & event_loop_p) : event_loop(&event_loop_p)
    {
        // constructor
    }

    rearm read_ready(eventloop_t &loop, int fd) noexcept
    {
        return receive_event(loop, fd, dasynq::IN_EVENTS);
    }
    
    rearm write_ready(eventloop_t &loop, int fd) noexcept
    {
        return receive_event(loop, fd, dasynq::OUT_EVENTS);
    }

    void set_watches(int flags)
    {
        eventloop_t::bidi_fd_watcher::set_watches(*event_loop, flags);
    }
};

inline dasynq::rearm control_conn_watcher::receive_event(eventloop_t &loop, int fd, int flags) noexcept
{
    return control_conn_cb(&loop, this, flags);
}


class control_conn_t : private service_listener
{
    friend rearm control_conn_cb(eventloop_t *loop, control_conn_watcher *watcher, int revents);
    
    control_conn_watcher iob;
    eventloop_t &loop;
    service_set *services;
    
    bool bad_conn_close = false; // close when finished output?
    bool oom_close = false;      // send final 'out of memory' indicator

    // The packet length before we need to re-check if the packet is complete.
    // process_packet() will not be called until the packet reaches this size.
    int chklen;
    
    // Receive buffer
    cpbuffer<1024> rbuf;
    
    template <typename T> using list = std::list<T>;
    template <typename T> using vector = std::vector<T>;
    
    // A mapping between service records and their associated numerical identifier used
    // in communction
    using handle_t = uint32_t;
    std::unordered_multimap<service_record *, handle_t> service_key_map;
    std::map<handle_t, service_record *> key_service_map;
    
    // Buffer for outgoing packets. Each outgoing back is represented as a vector<char>.
    list<vector<char>> outbuf;
    // Current index within the first outgoing packet (all previous bytes have been sent).
    unsigned outpkt_index = 0;
    
    // Queue a packet to be sent
    //  Returns:  false if the packet could not be queued and a suitable error packet
    //              could not be sent/queued (the connection should be closed);
    //            true (with bad_conn_close == false) if the packet was successfully
    //              queued;
    //            true (with bad_conn_close == true) if the packet was not successfully
    //              queued (but a suitable error packate has been queued).
    // The in/out watch enabled state will also be set appropriately.
    bool queue_packet(vector<char> &&v) noexcept;
    bool queue_packet(const char *pkt, unsigned size) noexcept;

    // Process a packet.
    //  Returns:  true (with bad_conn_close == false) if successful
    //            true (with bad_conn_close == true) if an error packet was queued
    //            false if an error occurred but no error packet could be queued
    //                (connection should be closed).
    // Throws:
    //    std::bad_alloc - if an out-of-memory condition prevents processing
    bool process_packet();
    
    // Process a STARTSERVICE/STOPSERVICE packet. May throw std::bad_alloc.
    bool process_start_stop(int pktType);
    
    // Process a FINDSERVICE/LOADSERVICE packet. May throw std::bad_alloc.
    bool process_find_load(int pktType);

    // Process an UNPINSERVICE packet. May throw std::bad_alloc.
    bool process_unpin_service();
    
    // Process an UNLOADSERVICE packet.
    bool process_unload_service();

    bool list_services();

    // Notify that data is ready to be read from the socket. Returns true if the connection should
    // be closed.
    bool data_ready() noexcept;
    
    bool send_data() noexcept;
    
    // Allocate a new handle for a service; may throw std::bad_alloc
    handle_t allocate_service_handle(service_record *record);
    
    service_record *find_service_for_key(uint32_t key)
    {
        try {
            return key_service_map.at(key);
        }
        catch (std::out_of_range &exc) {
            return nullptr;
        }
    }
    
    // Close connection due to out-of-memory condition.
    void do_oom_close()
    {
        bad_conn_close = true;
        oom_close = true;
        iob.set_watches(dasynq::OUT_EVENTS);
    }
    
    // Process service event broadcast.
    // Note that this can potentially be called during packet processing (upon issuing
    // service start/stop orders etc).
    void service_event(service_record * service, service_event_t event) noexcept final override
    {
        // For each service handle corresponding to the event, send an information packet.
        auto range = service_key_map.equal_range(service);
        auto & i = range.first;
        auto & end = range.second;
        try {
            while (i != end) {
                uint32_t key = i->second;
                std::vector<char> pkt;
                constexpr int pktsize = 3 + sizeof(key);
                pkt.reserve(pktsize);
                pkt.push_back(DINIT_IP_SERVICEEVENT);
                pkt.push_back(pktsize);
                char * p = (char *) &key;
                for (int j = 0; j < (int)sizeof(key); j++) {
                    pkt.push_back(*p++);
                }
                pkt.push_back(static_cast<char>(event));
                queue_packet(std::move(pkt));
                ++i;
            }
        }
        catch (std::bad_alloc &exc) {
            do_oom_close();
        }
    }
    
    public:
    control_conn_t(eventloop_t &loop, service_set * services_p, int fd)
            : iob(loop), loop(loop), services(services_p), chklen(0)
    {
        iob.add_watch(loop, fd, dasynq::IN_EVENTS);
        active_control_conns++;
    }
    
    bool rollback_complete() noexcept;
        
    virtual ~control_conn_t() noexcept;
};


static dasynq::rearm control_conn_cb(eventloop_t * loop, control_conn_watcher * watcher, int revents)
{
    // Get the address of the containing control_connt_t object:
    _Pragma ("GCC diagnostic push")
    _Pragma ("GCC diagnostic ignored \"-Winvalid-offsetof\"")
    char * cc_addr = (reinterpret_cast<char *>(watcher)) - offsetof(control_conn_t, iob);
    control_conn_t *conn = reinterpret_cast<control_conn_t *>(cc_addr);
    _Pragma ("GCC diagnostic pop")

    if (revents & dasynq::IN_EVENTS) {
        if (conn->data_ready()) {
            delete conn;
            return dasynq::rearm::REMOVED;
        }
    }
    if (revents & dasynq::OUT_EVENTS) {
        if (conn->send_data()) {
            delete conn;
            return dasynq::rearm::REMOVED;
        }
    }
    
    return dasynq::rearm::NOOP;
}

#endif
