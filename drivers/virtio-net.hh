/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef VIRTIO_NET_DRIVER_H
#define VIRTIO_NET_DRIVER_H

#include <bsd/porting/netport.h>
#include <bsd/sys/net/if_var.h>
#include <bsd/sys/net/if.h>
#include <bsd/sys/sys/mbuf.h>

#include "drivers/virtio.hh"
#include "drivers/pci-device.hh"
#include "osv/percpu.hh"
#include "lockfree/ring.hh"
#include "nway_merger.hh"

namespace virtio {

/**
 * virtio net device class
 */
class net : public virtio_driver {
public:

    friend struct txq;

    // The feature bitmap for virtio net
    enum NetFeatures {
        VIRTIO_NET_F_CSUM = 0,       /* Host handles pkts w/ partial csum */
        VIRTIO_NET_F_GUEST_CSUM = 1,       /* Guest handles pkts w/ partial csum */
        VIRTIO_NET_F_MAC = 5,       /* Host has given MAC address. */
        VIRTIO_NET_F_GSO = 6,       /* Host handles pkts w/ any GSO type */
        VIRTIO_NET_F_GUEST_TSO4 = 7,       /* Guest can handle TSOv4 in. */
        VIRTIO_NET_F_GUEST_TSO6 = 8,       /* Guest can handle TSOv6 in. */
        VIRTIO_NET_F_GUEST_ECN = 9,       /* Guest can handle TSO[6] w/ ECN in. */
        VIRTIO_NET_F_GUEST_UFO = 10,      /* Guest can handle UFO in. */
        VIRTIO_NET_F_HOST_TSO4 = 11,      /* Host can handle TSOv4 in. */
        VIRTIO_NET_F_HOST_TSO6 = 12,      /* Host can handle TSOv6 in. */
        VIRTIO_NET_F_HOST_ECN = 13,      /* Host can handle TSO[6] w/ ECN in. */
        VIRTIO_NET_F_HOST_UFO = 14,      /* Host can handle UFO in. */
        VIRTIO_NET_F_MRG_RXBUF = 15,      /* Host can merge receive buffers. */
        VIRTIO_NET_F_STATUS = 16,      /* net_config.status available */
        VIRTIO_NET_F_CTRL_VQ  = 17,      /* Control channel available */
        VIRTIO_NET_F_CTRL_RX = 18,      /* Control channel RX mode support */
        VIRTIO_NET_F_CTRL_VLAN = 19,      /* Control channel VLAN filtering */
        VIRTIO_NET_F_CTRL_RX_EXTRA = 20,   /* Extra RX mode control support */
        VIRTIO_NET_F_GUEST_ANNOUNCE = 21,  /* Guest can announce device on the network */
        VIRTIO_NET_F_MQ = 22,      /* Device supports Receive Flow Steering */
        VIRTIO_NET_F_CTRL_MAC_ADDR = 23,   /* Set MAC address */
    };

    enum {
        VIRTIO_NET_DEVICE_ID=0x1000,
        VIRTIO_NET_S_LINK_UP = 1,       /* Link is up */
        VIRTIO_NET_S_ANNOUNCE = 2,       /* Announcement is needed */
        VIRTIO_NET_OK = 0,
        VIRTIO_NET_ERR = 1,
        /*
         * Control the RX mode, ie. promisucous, allmulti, etc...
         * All commands require an "out" sg entry containing a 1 byte
         * state value, zero = disable, non-zero = enable.  Commands
         * 0 and 1 are supported with the VIRTIO_NET_F_CTRL_RX feature.
         * Commands 2-5 are added with VIRTIO_NET_F_CTRL_RX_EXTRA.
         */
        VIRTIO_NET_CTRL_RX = 0,
        VIRTIO_NET_CTRL_RX_PROMISC = 0,
        VIRTIO_NET_CTRL_RX_ALLMULTI = 1,
        VIRTIO_NET_CTRL_RX_ALLUNI = 2,
        VIRTIO_NET_CTRL_RX_NOMULTI = 3,
        VIRTIO_NET_CTRL_RX_NOUNI = 4,
        VIRTIO_NET_CTRL_RX_NOBCAST = 5,

        VIRTIO_NET_CTRL_MAC = 1,
        VIRTIO_NET_CTRL_MAC_TABLE_SET = 0,
        VIRTIO_NET_CTRL_MAC_ADDR_SET = 1,

        /*
         * Control VLAN filtering
         *
         * The VLAN filter table is controlled via a simple ADD/DEL interface.
         * VLAN IDs not added may be filterd by the hypervisor.  Del is the
         * opposite of add.  Both commands expect an out entry containing a 2
         * byte VLAN ID.  VLAN filterting is available with the
         * VIRTIO_NET_F_CTRL_VLAN feature bit.
         */
        VIRTIO_NET_CTRL_VLAN = 2,
        VIRTIO_NET_CTRL_VLAN_ADD = 0,
        VIRTIO_NET_CTRL_VLAN_DEL = 1,

        /*
         * Control link announce acknowledgement
         *
         * The command VIRTIO_NET_CTRL_ANNOUNCE_ACK is used to indicate that
         * driver has recevied the notification; device would clear the
         * VIRTIO_NET_S_ANNOUNCE bit in the status field after it receives
         * this command.
         */
        VIRTIO_NET_CTRL_ANNOUNCE = 3,
        VIRTIO_NET_CTRL_ANNOUNCE_ACK = 0,

        VIRTIO_NET_CTRL_MQ = 4,
        VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET = 0,
        VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MIN = 1,
        VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MAX = 0x8000,

        ETH_ALEN = 14,
        VIRTIO_NET_CSUM_OFFLOAD = CSUM_TCP | CSUM_UDP,
    };

    struct net_config {
        /* The config defining mac address (if VIRTIO_NET_F_MAC) */
        u8 mac[6];
        /* See VIRTIO_NET_F_STATUS and VIRTIO_NET_S_* above */
        u16 status;
        /* Maximum number of each of transmit and receive queues;
         * see VIRTIO_NET_F_MQ and VIRTIO_NET_CTRL_MQ.
         * Legal values are between 1 and 0x8000
         */
        u16 max_virtqueue_pairs;
    } __attribute__((packed));

    /* This is the first element of the scatter-gather list.  If you don't
     * specify GSO or CSUM features, you can simply ignore the header. */
    struct net_hdr {
        enum {
            VIRTIO_NET_HDR_F_NEEDS_CSUM  = 1,       // Use csum_start, csum_offset
            VIRTIO_NET_HDR_F_DATA_VALID = 2,       // Csum is valid
        };
        u8 flags;
        enum {
            VIRTIO_NET_HDR_GSO_NONE = 0,       // Not a GSO frame
            VIRTIO_NET_HDR_GSO_TCPV4 = 1,       // GSO frame, IPv4 TCP (TSO)
            VIRTIO_NET_HDR_GSO_UDP = 3,       // GSO frame, IPv4 UDP (UFO)
            VIRTIO_NET_HDR_GSO_TCPV6 = 4,       // GSO frame, IPv6 TCP
            VIRTIO_NET_HDR_GSO_ECN = 0x80,    // TCP has ECN set
        };
        u8 gso_type;
        u16 hdr_len;          /* Ethernet + IP + tcp/udp hdrs */
        u16 gso_size;         /* Bytes to append to hdr_len per frame */
        u16 csum_start;       /* Position to start checksumming from */
        u16 csum_offset;      /* Offset after that to place checksum */
    };

    /* This is the version of the header to use when the MRG_RXBUF
     * feature has been negotiated. */
    struct net_hdr_mrg_rxbuf {
        struct net_hdr hdr;
        u16 num_buffers;      /* Number of merged rx buffers */
    };

    /*
     * Control virtqueue data structures
     *
     * The control virtqueue expects a header in the first sg entry
     * and an ack/status response in the last entry.  Data for the
     * command goes in between.
     */
    struct net_ctrl_hdr {
            u8 class_t;
            u8 cmd;
    } __attribute__((packed));

    typedef u8 net_ctrl_ack;

    /*
     * Control the MAC
     *
     * The MAC filter table is managed by the hypervisor, the guest should
     * assume the size is infinite.  Filtering should be considered
     * non-perfect, ie. based on hypervisor resources, the guest may
     * received packets from sources not specified in the filter list.
     *
     * In addition to the class/cmd header, the TABLE_SET command requires
     * two out scatterlists.  Each contains a 4 byte count of entries followed
     * by a concatenated byte stream of the ETH_ALEN MAC addresses.  The
     * first sg list contains unicast addresses, the second is for multicast.
     * This functionality is present if the VIRTIO_NET_F_CTRL_RX feature
     * is available.
     *
     * The ADDR_SET command requests one out scatterlist, it contains a
     * 6 bytes MAC address. This functionality is present if the
     * VIRTIO_NET_F_CTRL_MAC_ADDR feature is available.
     */
    struct net_ctrl_mac {
            u32 entries;
            u8 macs[][ETH_ALEN];
    } __attribute__((packed));

    /*
     * Control Receive Flow Steering
     *
     * The command VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET
     * enables Receive Flow Steering, specifying the number of the transmit and
     * receive queues that will be used. After the command is consumed and acked by
     * the device, the device will not steer new packets on receive virtqueues
     * other than specified nor read from transmit virtqueues other than specified.
     * Accordingly, driver should not transmit new packets  on virtqueues other than
     * specified.
     */
    struct net_ctrl_mq {
            u16 virtqueue_pairs;
    };

    explicit net(pci::device& dev);
    virtual ~net();

    virtual const std::string get_name(void) { return _driver_name; }
    bool read_config();

    virtual u32 get_driver_features(void);

    void wait_for_queue(vring* queue);
    bool bad_rx_csum(struct mbuf *m, struct net_hdr *hdr);
    void receiver();
    void fill_rx_ring();

    void kick(int queue) {_queues[queue]->kick();}
    static hw_driver* probe(hw_device* dev);

    /**
     * Fill the if_data buffer with data from our iface including those that
     * we have gathered by ourselvs (e.g. FP queue stats).
     * @param out_data output buffer
     */
    void fill_stats(struct if_data* out_data) const;

    int xmit(struct mbuf* buff);
private:

    struct net_req {
        net_req() { memset(&mhdr, 0, sizeof(mhdr)); };
        net_req(mbuf *m) {
            memset(&mhdr, 0, sizeof(mhdr));
            um.reset(m);
        }

        struct net::net_hdr_mrg_rxbuf mhdr;
        struct free_deleter {
            void operator()(mbuf *m) { m_freem(m); }
        };

        std::unique_ptr<mbuf, free_deleter> um;

    };

    std::string _driver_name;
    net_config _config;

    // TODO: Consider moving all these to txq
    bool _mergeable_bufs;
    bool _tso_ecn = false;
    bool _status = false;
    bool _host_tso_ecn = false;
    bool _csum = false;
    bool _guest_csum = false;
    bool _guest_tso4 = false;
    bool _host_tso4 = false;
    bool _guest_ufo = false;

    u32 _hdr_size;

    struct rxq_stats {
        u64 rx_packets; /* if_ipackets */
        u64 rx_bytes;   /* if_ibytes */
        u64 rx_drops;   /* if_iqdrops */
        u64 rx_csum;    /* number of packets with correct csum */
        u64 rx_csum_err;/* number of packets with a bad checksum */
    };

    struct txq_stats {
        u64 tx_packets; /* if_opackets */
        u64 tx_bytes;   /* if_obytes */
        u64 tx_err;     /* Number of broken packets */
        u64 tx_drops;   /* Number of dropped packets */
        u64 tx_csum;    /* CSUM offload requests */
        u64 tx_tso;     /* GSO/TSO packets */
        /* u64 tx_rescheduled; */ /* TODO when we implement xoff */
        u64 tx_kicks;
        u64 tx_pkts_from_disp;
        u64 tx_disp_wakeups;
    };

     /* Single Rx queue object */
    struct rxq {
        rxq(vring* vq, std::function<void ()> poll_func)
            : vqueue(vq), poll_task(poll_func) {};
        vring* vqueue;
        sched::thread  poll_task;
        struct rxq_stats stats = { 0 };
    };

    /* optionally: use std::pair */
    struct tx_buff_desc {
        mbuf* buf;
        s64 ts;

        bool operator>(const tx_buff_desc& other) const
        {
            return ts - other.ts > 0;
        }
    };

    class tx_cpu_queue {
    public:
        typedef tx_buff_desc T;

        class tx_queue_iterator {
        public:

            T& operator *() const { return _r->_r.front(); }

        private:
            friend class tx_cpu_queue;
            explicit tx_queue_iterator(tx_cpu_queue* r) : _r(r) { }
            tx_cpu_queue* _r;
        };

        typedef tx_queue_iterator      iterator;

        explicit tx_cpu_queue(unsigned sz) : _r(sz) { }

        T& front() { return _r.front(); }
        iterator begin() { return iterator(this); }

        bool push(T v) { return _r.push(v); }

        void erase(iterator &it) {
            T tmp;
            _r.pop(tmp);
        }

        bool empty() const { return _r.empty(); }

        unsigned size() const { return _r.size(); }

        u64 tx_dropped = 0;
    private:
        ring_spsc<tx_buff_desc> _r;
    };

    struct txq;
    class tx_xmit_iterator {
    public:
        tx_xmit_iterator(txq* txq) : _q(txq) { }

        // These ones will do nothing
        tx_xmit_iterator& operator *() { return *this; }
        tx_xmit_iterator& operator++() { return *this; }

        /**
         * Push the packet downstream
         * @param tx_desc
         */
        void operator=(const tx_buff_desc& tx_desc) {
            //std::cout<<"Sending packet with ts "<<tx_desc.ts<<std::endl;
            int error = _q->xmit_one_locked(tx_desc.buf);

            if (error) {
                // Hmmm... Bad packet?!
                assert(0);
            }
        }
    private:
        txq* _q;
    };

    /* Single Tx queue object TODO: Make it a class */
    struct txq {
        /**
         * This is the size of the buffers ring of the FreeBSD virtio-net
         * driver. So, I'm using this as a baseline. We may ajust this value
         * later (cut it down maybe?!).
         *
         * Currently this gives us ~16 pages per one CPU ring.
         */
        const unsigned cpu_txq_size	= 4096;

        txq(net* parent, vring* vq) :
            vqueue(vq),
            dispatcher_task([this] { dispatch(); }),
            mg([this] { return !has_pending(); }),
            xmit_it(this), _check_empty_queues(false), _parent(parent)
        {
            for (auto c : sched::cpus) {
                cpuq.for_cpu(c)->
                    reset(new tx_cpu_queue(cpu_txq_size));
            }
        };

        /**
         * Try to transmit a single packet. Don't block on failure.
         *
         * Must run with "running" lock taken.
         * @param req
         * @param tx_bytes
         *
         * @return 0 if packet has been successfully sent, EINVAL if a packet is
         *         not well-formed and ENOBUFS if there was no room on a HW ring
         *         to send the packet.
         */
        int try_xmit_one_locked(net_req *req, u64& tx_bytes);

        /**
         * Transmit a single packet. Will wait for completions if there is no
         * room on a HW ring.
         *
         * Must run with "running" lock taken.
         * @param m_head a buffer to transmits
         *
         * @return 0 in case of success and an appropriate error code
         *         otherwise.
         */
        int xmit_one_locked(mbuf *m_head);

        /**
         * A main xmit function: will try to bypass the per-CPU queue if
         * possible and will push the frame into that queue otherwise.
         *
         * Either ways it won't block.
         * @param buf packet descriptor to send
         *
         * @return 0 in case of success, EINVAL if a packet is not well-formed
         *         and ENOBUFS if there was no room (on HW or CPU ring) to send
         *         the packet.
         */
        int xmit(mbuf *buf);

        /**
         * Push the packet into the per-CPU queue for the current CPU.
         * @param buf packet descriptor to push
         *
         * @return
         */
        int push_cpu(mbuf *buf);

        /**
         * Free the descriptors for the completed packets.
         */
        void gc();

        /* TODO: deplete the per-cpu rings in ~txq() and in if_qflush() */

        vring* vqueue;
        struct txq_stats stats = { 0 };
        dynamic_percpu<std::unique_ptr<tx_cpu_queue> > cpuq;
        sched::thread dispatcher_task;
        sched::thread_handle new_work_hdl, running_hdl;

        osv::nway_merger<std::list<tx_cpu_queue*> > mg;
        tx_xmit_iterator xmit_it;
        u16 pkts_to_kick = 0;
        /**
         * This lock will be used to get an exclusive control over the HW
         * channel.
         */
        std::atomic_flag running = ATOMIC_FLAG_INIT;
        //int state = 0;
    private:
        void dispatch();
        void bh_func();
        void kick();
        struct mbuf* tx_offload(struct mbuf* m, struct net_hdr* hdr);
        void update_stats(net_req* req, u64 tx_bytes);

        // RUNNING state controling functions
        bool try_lock_running();
        void lock_running();
        void unlock_running();

        // PENDING (packets) controling functions
        bool has_pending() const;
        bool test_and_set_pending();
        void clear_pending();

        std::atomic<bool> _check_empty_queues;
        net* _parent;
    };

    /**
     * Fill the Rx queue statistics in the general info struct
     * @param rxq Rx queue handle
     * @param out_data output buffer
     */
    void fill_qstats(const struct rxq& rxq, struct if_data* out_data) const;

    /**
     * Fill the Tx queue statistics in the general info struct
     * @param txq Tx queue handle
     * @param out_data output buffer
     */
    void fill_qstats(const struct txq& txq, struct if_data* out_data) const;

    /* We currently support only a single Rx+Tx queue */
    struct rxq _rxq;
    struct txq _txq;

    //maintains the virtio instance number for multiple drives
    static int _instance;
    int _id;
    struct ifnet* _ifn;
};

}

#endif

