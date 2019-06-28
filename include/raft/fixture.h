/**
 * Raft cluster test fixture, using an in-memory @raft_io implementation. This
 * is meant to be used in unit tests.
 */

#ifndef RAFT_FIXTURE_H
#define RAFT_FIXTURE_H

#include "../raft.h"

#define RAFT_FIXTURE_MAX_SERVERS 8

/**
 * Fixture step event types;
 */
enum {
    RAFT_FIXTURE_TICK = 1, /* The tick callback has been invoked */
    RAFT_FIXTURE_NETWORK,  /* A network request has been sent or received */
    RAFT_FIXTURE_DISK      /* An I/O request has been submitted */
};

struct raft_fixture_server
{
    bool alive;
    unsigned id;
    char address[8];
    struct raft_logger logger;
    struct raft_io io;
    struct raft raft;
};

/**
 * Information about a test cluster event triggered by the fixture.
 */
struct raft_fixture_event
{
    unsigned server_index; /* Server index the event was fired on */
    int type;              /* Type of the event that was fired */
};

/**
 * Event callback. See raft_fixture_hook().
 */
struct raft_fixture;
typedef void (*raft_fixture_event_cb)(struct raft_fixture *f,
                                      struct raft_fixture_event *event);

/**
 * Test implementation of a cluster of N servers, each having a user-provided
 * FSM.
 *
 * Out of the N servers, there can be V voting servers, with V <= N.
 *
 * The cluster can simulate network latency and time elapsed on individual
 * servers.
 *
 * Servers can be alive or dead. Network messages sent to dead servers are
 * dropped. Dead servers do not have their @raft_io_tick_cb callback invoked.
 *
 * Any two servers can be connected or disconnected. Network messages sent
 * between disconnected servers are dropped.
 */
struct raft_fixture
{
    raft_time time;                  /* Number of milliseconds elapsed. */
    unsigned n;                      /* Number of servers */
    unsigned leader_id;              /* ID of current leader, or 0 */
    struct raft_log log;             /* Copy of leader's log */
    raft_index commit_index;         /* Current commit index on leader */
    struct raft_fixture_event event; /* Last event */
    raft_fixture_event_cb hook;      /* Event callback */
    struct raft_fixture_server servers[RAFT_FIXTURE_MAX_SERVERS];
};

/**
 * Initialize a raft cluster fixture with @n servers. Each server will use an
 * in-memory @raft_io implementation and one of the given @fsms. All servers
 * will be initially connected to one another, but they won't be bootstrapped or
 * started.
 */
int raft_fixture_init(struct raft_fixture *f,
                      unsigned n,
                      struct raft_fsm *fsms);

/**
 * Release all memory used by the fixture.
 */
void raft_fixture_close(struct raft_fixture *f);

/**
 * Generate a configuration object containing all servers in the cluster. The
 * first @n_voting servers will be voting ones.
 */
int raft_fixture_configuration(struct raft_fixture *f,
                               unsigned n_voting,
                               struct raft_configuration *configuration);

/**
 * Bootstrap all servers in the cluster with the given configuration.
 */
int raft_fixture_bootstrap(struct raft_fixture *f,
                           struct raft_configuration *configuration);

/**
 * Start all servers in the fixture.
 */
int raft_fixture_start(struct raft_fixture *f);

/**
 * Return the current number of servers in the fixture.
 */
unsigned raft_fixture_n(struct raft_fixture *f);

/**
 * Return the current cluster time. All raft instances see the same exactly
 * synchronized time.
 */
raft_time raft_fixture_time(struct raft_fixture *f);

/**
 * Return the raft instance associated with the i'th server of the fixture.
 */
struct raft *raft_fixture_get(struct raft_fixture *f, unsigned i);

/**
 * Return @true if the i'th server hasn't been killed.
 */
bool raft_fixture_alive(struct raft_fixture *f, unsigned i);

/**
 * Return the index of the current leader, or the current number of servers if
 * there's no leader.
 */
unsigned raft_fixture_leader_index(struct raft_fixture *f);

/**
 * Return the ID of the server the @i'th server has voted for, or zero.
 */
unsigned raft_fixture_voted_for(struct raft_fixture *f, unsigned i);

/**
 * Drive the cluster so the @i'th server gets elected as leader.
 *
 * This is achieved by bumping the randomized election timeout of all other
 * servers to a very high value, letting the one of the @i'th server expire and
 * then stepping the cluster until the election is won.
 *
 * There must currently be no leader and no candidate and the given server must
 * be a voting one. Also, the @i'th server must be connected to a majority of
 * voting servers.
 */
void raft_fixture_elect(struct raft_fixture *f, unsigned i);

/**
 * Drive the cluster so the current leader gets deposed.
 *
 * This is achieved by dropping all AppendEntries result messages sent by
 * followers to the leader, until the leader decides to step down.
 */
void raft_fixture_depose(struct raft_fixture *f);

/**
 * Step through the cluster state advancing the time to the minimum value needed
 * for it to make progress (i.e. for a message to be delivered, for an I/O
 * operation to complete or for a server time out).
 *
 * In particular, the following happens:
 *
 * 1. If there are pending #raft_io_send, the oldest one is picked and the
 *    relevant callback fired. This simulates completion of a socket write. It
 *    does not mean that the receiver immediately receives the message: it just
 *    means that any buffer allocated by the sender can be released (e.g. log
 *    entries). The in-memory I/O implementation assigns a latency to each RPC
 *    message, which will get delivered to the receiver only after that amount
 *    of time elapses. If the sender and the receiver are currently
 *    disconnected, the RPC message is simply dropped. If a callback was fired,
 *    jump directly to 3. and skip 2.
 *
 * 2. All pending disk I/O requests across all servers are scanned and the one
 *    with the lowest completion time is picked. All network messages across all
 *    servers are scanned and the one with the lowest delivery time is
 *    picked. All servers are scanned, and the one with the lowest tick
 *    expiration time is picked. The three times are compared and the lowest one
 *    is picked. If a disk I/O request has completed, the relevant callback will
 *    be invoked, if there's an RPC to be delivered, the receiver's
 *    @raft_io_recv_cb callback gets fired, if a tick timer has expired the
 *    relevant tick callback will be invoked. Only one event will be fired. If
 *    there is more than one event to fire, one of them is picked according to
 *    the following rules: events for servers with lower index are fired first,
 *    tick events take precedence over disk events, and disk events take
 *    precedence over network events.
 *
 * 3. The current cluster leader is detected (if any). When detecting the leader
 *    the Election Safety property is checked: no servers can be in leader state
 *    for the same term. The server in leader state with the highest term is
 *    considered the current cluster leader, as long as it's "stable", i.e. it
 *    has been acknowledged by all servers connected to it, and those servers
 *    form a majority (this means that no further leader change can happen,
 *    unless the network gets disrupted). If there is a stable leader and it has
 *    not changed with respect to the previous call to @raft_fixture_step(),
 *    then the Leader Append-Only property is checked, by comparing its log with
 *    a copy of it that was taken during the previous iteration.
 *
 * 4. If there is a stable leader, its current log is copied, in order to be
 *    able to check the Leader Append-Only property at the next call.
 *
 * 5. If there is a stable leader, its commit index gets copied.
 *
 */
struct raft_fixture_event *raft_fixture_step(struct raft_fixture *f);

/**
 * Call raft_fixture_step() exactly @n times, and return the last event fired.
 */
struct raft_fixture_event *raft_fixture_step_n(struct raft_fixture *f,
                                               unsigned n);

/**
 * Step the cluster until the given @stop function returns #true, or @max_msecs
 * have elapsed.
 *
 * Return #true if the @stop function has returned #true within @max_msecs.
 */
bool raft_fixture_step_until(struct raft_fixture *f,
                             bool (*stop)(struct raft_fixture *f, void *arg),
                             void *arg,
                             unsigned max_msecs);

/**
 * Step the cluster until @msecs have elapsed.
 */
void raft_fixture_step_until_elapsed(struct raft_fixture *f, unsigned msecs);

/**
 * Step the cluster until a leader is elected, or @max_msecs have elapsed.
 */
bool raft_fixture_step_until_has_leader(struct raft_fixture *f,
                                        unsigned max_msecs);

/**
 * Step the cluster until the current leader gets deposed, or @max_msecs have
 * elapsed.
 */
bool raft_fixture_step_until_has_no_leader(struct raft_fixture *f,
                                           unsigned max_msecs);

/**
 * Step the cluster until the @i'th server has applied the entry at the given
 * index, or @max_msecs have elapsed. If @i equals the number of servers, then
 * step until all servers have applied the given entry.
 */
bool raft_fixture_step_until_applied(struct raft_fixture *f,
                                     unsigned i,
                                     raft_index index,
                                     unsigned max_msecs);

/**
 * Step the cluster until the state of the @i'th server matches the given one,
 * or @max_msecs have elapsed.
 */
bool raft_fixture_step_until_state_is(struct raft_fixture *f,
                                      unsigned i,
                                      int state,
                                      unsigned max_msecs);

/**
 * Step the cluster until the term of the @i'th server matches the given one,
 * or @max_msecs have elapsed.
 */
bool raft_fixture_step_until_term_is(struct raft_fixture *f,
                                     unsigned i,
                                     raft_term term,
                                     unsigned max_msecs);

/**
 * Step the cluster until the @i'th server has voted for the @j'th one, or
 * @max_msecs have elapsed.
 */
bool raft_fixture_step_until_voted_for(struct raft_fixture *f,
                                       unsigned i,
                                       unsigned j,
                                       unsigned max_msecs);

/**
 * Step the cluster all messages from the @i'th server to the @j'th server have
 * been delivered, or @max_msecs have elapsed.
 */
bool raft_fixture_step_until_delivered(struct raft_fixture *f,
                                       unsigned i,
                                       unsigned j,
                                       unsigned max_msecs);

/**
 * Set a function to be called after every time a fixture event occurs as
 * consequence of a step.
 */
void raft_fixture_hook(struct raft_fixture *f, raft_fixture_event_cb hook);

/**
 * Disconnect the @i'th and the @j'th servers, so attempts to send a message
 * from @i to @j will fail with #RAFT_NOCONNECTION.
 */
void raft_fixture_disconnect(struct raft_fixture *f, unsigned i, unsigned j);

/**
 * Reconnect the @i'th and the @j'th servers, so attempts to send a message
 * from @i to @j will succeed again.
 */
void raft_fixture_reconnect(struct raft_fixture *f, unsigned i, unsigned j);

/**
 * Saturate the connection between the @i'th and the @j'th servers, so messages
 * sent by @i to @j will be silently dropped.
 */
void raft_fixture_saturate(struct raft_fixture *f, unsigned i, unsigned j);

/**
 * Return true if the connection from the @i'th to the @j'th server has been set
 * as saturated.
 */
bool raft_fixture_saturated(struct raft_fixture *f, unsigned i, unsigned j);

/**
 * Desaturate the connection between the @i'th and the @j'th servers, so
 * messages sent by @i to @j will start being delivered again.
 */
void raft_fixture_desaturate(struct raft_fixture *f, unsigned i, unsigned j);

/**
 * Kill the server with the given index. The server won't receive any message
 * and its tick callback won't be invoked.
 */
void raft_fixture_kill(struct raft_fixture *f, unsigned i);

/**
 * Add a new empty server to the cluster and connect it to all others.
 */
int raft_fixture_grow(struct raft_fixture *f, struct raft_fsm *fsm);

/**
 * Set the value that will be returned to the @i'th raft instance when it asks
 * the underlying raft_io implementation for a randomized election timeout
 * value. The default value is 1000 + @i * 100, meaning that the timer of server
 * 0 will expire first.
 */
void raft_fixture_set_randomized_election_timeout(struct raft_fixture *f,
                                                  unsigned i,
                                                  unsigned msecs);

/**
 * Set the network latency in milliseconds. Each RPC message sent by the @i'th
 * server from now on will take @msecs milliseconds to be delivered. The default
 * value is 15.
 */
void raft_fixture_set_network_latency(struct raft_fixture *f,
                                      unsigned i,
                                      unsigned msecs);

/**
 * Set the disk I/O latency in milliseconds. Each append request will take this
 * amount of milliseconds to complete. The default value is 10.
 */
void raft_fixture_set_disk_latency(struct raft_fixture *f,
                                   unsigned i,
                                   unsigned msecs);

/**
 * Set the persisted term of the @i'th server.
 */
void raft_fixture_set_term(struct raft_fixture *f, unsigned i, raft_term term);

/**
 * Set the persisted snapshot of the @i'th server.
 */
void raft_fixture_set_snapshot(struct raft_fixture *f,
                               unsigned i,
                               struct raft_snapshot *snapshot);

/**
 * Set the persisted entries of the @i'th server.
 */
void raft_fixture_set_entries(struct raft_fixture *f,
                              unsigned i,
                              struct raft_entry *entries,
                              unsigned n);

/**
 * Add an entry to the persisted entries of the @i'th server.
 */
void raft_fixture_add_entry(struct raft_fixture *f,
                            unsigned i,
                            struct raft_entry *entry);

/**
 * Inject an I/O failure that will be triggered on the @i'th server after @delay
 * I/O requests and occur @repeat times.
 */
void raft_fixture_io_fault(struct raft_fixture *f,
                           unsigned i,
                           int delay,
                           int repeat);

/**
 * Return the number of messages of the given type that the @i'th server has
 * successfully sent so far.
 */
unsigned raft_fixture_n_send(struct raft_fixture *f, unsigned i, int type);

/**
 * Return the number of messages of the given type that the @i'th server has
 * received so far.
 */
unsigned raft_fixture_n_recv(struct raft_fixture *f, unsigned i, int type);

#endif /* RAFT_FIXTURE_H */