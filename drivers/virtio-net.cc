/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#include <sys/cdefs.h>

#include "drivers/virtio.hh"
#include "drivers/virtio-net.hh"
#include "drivers/pci-device.hh"
#include "interrupt.hh"

#include "mempool.hh"
#include "mmu.hh"

#include <string>
#include <string.h>
#include <map>
#include <atomic>
#include <errno.h>
#include <osv/debug.h>

#include "sched.hh"
#include "osv/trace.hh"

#include "drivers/clock.hh"
#include "drivers/clockevent.hh"

#include <osv/device.h>
#include <osv/ioctl.h>
#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/net/if_types.h>
#include <bsd/sys/sys/param.h>

#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/net/if_vlan_var.h>
#include <bsd/sys/netinet/in.h>
#include <bsd/sys/netinet/ip.h>
#include <bsd/sys/netinet/udp.h>
#include <bsd/sys/netinet/tcp.h>

TRACEPOINT(trace_virtio_net_rx_packet, "if=%d, len=%d", int, int);
TRACEPOINT(trace_virtio_net_rx_wake, "");
TRACEPOINT(trace_virtio_net_fill_rx_ring, "if=%d", int);
TRACEPOINT(trace_virtio_net_fill_rx_ring_added, "if=%d, added=%d", int, int);
TRACEPOINT(trace_virtio_net_tx_packet, "if=%d, len=%d", int, int);
TRACEPOINT(trace_virtio_net_tx_failed_add_buf, "if=%d", int);
TRACEPOINT(trace_virtio_net_tx_no_space_calling_gc, "if=%d", int);
using namespace memory;

// TODO list
// irq thread affinity and tx affinity
// tx zero copy
// vlans?

namespace virtio {

int net::_instance = 0;

#define net_tag "virtio-net"
#define net_d(...)   tprintf_d(net_tag, __VA_ARGS__)
#define net_i(...)   tprintf_i(net_tag, __VA_ARGS__)
#define net_w(...)   tprintf_w(net_tag, __VA_ARGS__)
#define net_e(...)   tprintf_e(net_tag, __VA_ARGS__)

static int if_ioctl(struct ifnet *ifp, u_long command, caddr_t data)
{
    net_d("if_ioctl %x", command);

    int error = 0;
    switch(command) {
    case SIOCSIFMTU:
        net_d("SIOCSIFMTU");
        break;
    case SIOCSIFFLAGS:
        net_d("SIOCSIFFLAGS");
        /* Change status ifup, ifdown */
        if (ifp->if_flags & IFF_UP) {
            ifp->if_drv_flags |= IFF_DRV_RUNNING;
            net_d("if_up");
        } else {
            ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
            net_d("if_down");
        }
        break;
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        net_d("SIOCDELMULTI");
        break;
    default:
        net_d("redirecting to ether_ioctl()...");
        error = ether_ioctl(ifp, command, data);
        break;
    }

    return(error);
}

/**
 * Invalidate the local Tx queues.
 * @param ifp upper layer instance handle
 */
static void if_qflush(struct ifnet *ifp)
{
    //
    // TODO: Add per-CPU Tx queues flushing here. Most easily checked with
    // change MTU use case.
    //
    ::if_qflush(ifp);
}

/**
 * Transmits a single mbuf instance.
 * @param ifp upper layer instance handle
 * @param m_head mbuf to transmit
 *
 * @return 0 in case of success and an appropriate error code
 *         otherwise
 */
static int if_transmit(struct ifnet* ifp, struct mbuf* m_head)
{
    net* vnet = (net*)ifp->if_softc;

    return vnet->xmit(m_head);
}

int net::xmit(struct mbuf* buff)
{
    //
    // We currently have only a single TX queue. Select a proper TXq here when
    // we implement a multi-queue.
    //
    return _txq.xmit(buff);
}

int net::txq::xmit(mbuf* buff)
{
    //
    // If there are pending packets (in the per-CPU queues) or we've failed to
    // take a RUNNING lock push the packet in the per-CPU queue.
    //
    // Otherwise means that a dispatcher is neither running nor is
    // scheduled to run. In this case bypass per-CPU queues and transmit
    // in-place.
    //
    if (has_pending() || !try_lock_running()) {
        push_cpu(buff);
        return 0;
    }

    int rc = 0;
    net_req *req = new net_req(buff);
    u64 tx_bytes;

    // If we are here means we've aquired a RUNNING lock
    rc = try_xmit_one_locked(req, tx_bytes);

    // Alright!!!
    if (!rc) {
        update_stats(req, tx_bytes);
        stats.tx_kicks++;
        if (vqueue->kick()) {
            stats.tx_hv_kicks++;
        }
    }

    unlock_running();

    //
    // We unlock_running() not from a dispatcher only if the dispatcher is not
    // running and is waiting for either a new work or for this lock.
    //
    // We want to wake a dispatcher only if there is a new work for it since
    // otherwise there is no point for it to wake up.
    //
    if (has_pending()) {
        dispatcher_task.wake();
    }

    if (rc == EINVAL) {
        // The packet is f...d - drop it!
        req->free_mbuf();
        delete req;
    } else if (rc) {
        //
        // There hasn't been enough buffers on the HW ring to send the
        // packet - push it into the per-CPU queue, dispatcher will handle
        // it later.
        //
        rc = 0;
        push_cpu(buff);

        delete req;
    }

    return rc;
}

void net::txq::push_cpu(mbuf* buff)
{
    bool success = false;

    sched::preempt_disable();

    tx_buff_desc new_buff_desc = { buff, get_ts() };
    tx_cpu_queue* local_cpuq = cpuq->get();

    while (!local_cpuq->push(new_buff_desc)) {
        wait_record wr(sched::thread::current());
        local_cpuq->push_new_waiter(&wr);

        //
        // Try to push again in order to resolve a nasty race:
        //
        // If dispatcher has succeeded to empty the whole ring before to added
        // our record to the waitq then without this push() we could have stuck
        // until somebody else adds another packet to this specific cpuq. In
        // this case adding a packet will ensure that dispatcher eventually
        // handles it and "wake()s" us up.
        //
        // If we fail to add the packet here then this means that the queue has
        // still been full AFTER we added the wait_record and we need to wait
        // until dispatcher cleans it up and wakes us.
        //
        // All this is because we can't exit this function until dispatcher
        // pop()s our wait_record since it's allocated on our stack.
        //
        success = local_cpuq->push(new_buff_desc);
        if (success && !test_and_set_pending()) {
            dispatcher_task.wake();
        }

        sched::preempt_enable();

        wr.wait();

        // we are done - get out!
        if (success) {
            return;
        }

        sched::preempt_disable();

        // Refresh: we could have been moved to a different CPU
        local_cpuq = cpuq->get();
        //
        // Refresh: another thread could have pushed its packet before us and it
        //          had an earlier timestamp - we have to keep the timestampes
        //          ordered in the CPU queue.
        //
        new_buff_desc.ts = get_ts();
    }

    //
    // Save the IPI sending (when dispatcher sleeps for an interrupt) and
    // exchange in the wake_impl() by paying a price of an exchange operation
    // here.
    //
    if (!test_and_set_pending()) {
        dispatcher_task.wake();
    }

    sched::preempt_enable();

    return;
}

inline void net::txq::kick()
{
    if (pkts_to_kick) {
        stats.tx_pkts_from_disp += pkts_to_kick;
        pkts_to_kick = 0;
        stats.tx_kicks++;
        if (vqueue->kick()) {
            stats.tx_hv_kicks++;
        }
    }
}

inline bool net::txq::try_lock_running()
{
    return !running.test_and_set(std::memory_order_acquire);

}

inline void net::txq::lock_running()
{
    //
    // Check if there is no fast-transmit hook running already.
    // If yes - sleep until it ends.
    //
    if (!try_lock_running()) {
        sched::thread::wait_until([this] { return try_lock_running(); });
    }
}

inline void net::txq::unlock_running()
{
    running.clear(std::memory_order_release);
}

inline bool net::txq::has_pending() const
{
    return _check_empty_queues.load(std::memory_order_acquire);
}

inline bool net::txq::test_and_set_pending()
{
    return _check_empty_queues.exchange(true, std::memory_order_acq_rel);
}

inline void net::txq::clear_pending()
{
    _check_empty_queues.store(false, std::memory_order_release);
}

void net::txq::dispatch()
{
    //
    // Kick at least every full ring of packets.
    // Othersize a deadlock is possible:
    //   1) We post a full ring of buffers without a kick().
    //   2) We block on posting of the next buffer.
    //   3) HW doesn't know there is a work to do.
    //   4) Dead lock.
    //
    const int kick_thresh = vqueue->size();

    // Create a collection of a per-CPU queues
    std::list<tx_cpu_queue*> all_cpuqs;

    for (auto c : sched::cpus) {
        all_cpuqs.push_back(cpuq.for_cpu(c)->get());
    }

    // Push them all into the heap
    mg.create_heap(all_cpuqs);

    //
    // Dispatcher holds the RUNNING lock all the time it doesn't sleep waiting
    // for a new work.
    //
    lock_running();

    // Start taking packets one-by-one and send them out
    while (1) {
        //
        // Reset the PENDING state.
        //
        // The producer thread will first add a new element to the heap and only
        // then set the PENDING state.
        //
        clear_pending();

        // Check if there are elements in the heap
        if (!mg.pop(xmit_it)) {
            // Wake all unwoken waiters before going to sleep
            wake_waiters_all();

            // We are going to sleep - release the HW channel
            unlock_running();

            sched::thread::wait_until([this] { return has_pending(); });
            stats.tx_disp_wakeups++;

            lock_running();
        }

        while (mg.pop(xmit_it)) {
            if (pkts_to_kick >= kick_thresh) {
                kick();
            }
        }

        kick();
    }
}

static void if_init(void* xsc)
{
    net_d("Virtio-net init");
}

/**
 * Return all the statistics we have gathered.
 * @param ifp
 * @param out_data
 */
static void if_getinfo(struct ifnet* ifp, struct if_data* out_data)
{
    net* vnet = (net*)ifp->if_softc;

    // First - take the ifnet data
    memcpy(out_data, &ifp->if_data, sizeof(*out_data));

    // then fill the internal statistics we've gathered
    vnet->fill_stats(out_data);
}

void net::fill_stats(struct if_data* out_data) const
{
    // We currently support only a single Tx/Rx queue so no iteration so far
    fill_qstats(_rxq, out_data);
    fill_qstats(_txq, out_data);
}

void net::fill_qstats(const struct rxq& rxq,
                             struct if_data* out_data) const
{
    out_data->ifi_ipackets += rxq.stats.rx_packets;
    out_data->ifi_ibytes   += rxq.stats.rx_bytes;
    out_data->ifi_iqdrops  += rxq.stats.rx_drops;
    out_data->ifi_ierrors  += rxq.stats.rx_csum_err;
}

void net::fill_qstats(const struct txq& txq,
                      struct if_data* out_data) const
{
#ifdef TX_DEBUG
    printf("pkts(%d)/kicks(%d)=%f\n", txq.stats.tx_packets,
           txq.stats.tx_hv_kicks,
           (double)txq.stats.tx_packets/txq.stats.tx_hv_kicks);
    printf("hv_kicks(%d)/kicks(%d)=%f\n", txq.stats.tx_hv_kicks,
           txq.stats.tx_kicks,
           (double)txq.stats.tx_hv_kicks/txq.stats.tx_kicks);
    printf("disp_pkts(%d)/disp_wakeups(%d) = %f\n", txq.stats.tx_pkts_from_disp,
           txq.stats.tx_disp_wakeups,
           (double)txq.stats.tx_pkts_from_disp/txq.stats.tx_disp_wakeups);
#endif
    assert(!out_data->ifi_oerrors && !out_data->ifi_obytes && !out_data->ifi_opackets);
    out_data->ifi_opackets += txq.stats.tx_packets;
    out_data->ifi_obytes   += txq.stats.tx_bytes;
    out_data->ifi_oerrors  += txq.stats.tx_err + txq.stats.tx_drops;
}

net::net(pci::device& dev)
    : virtio_driver(dev),
      _rxq(get_virt_queue(0), [this] { this->receiver(); }),
      _txq(this, get_virt_queue(1))
{
    sched::thread* poll_task = &_rxq.poll_task;
    sched::thread* tx_dispatcher_task = &_txq.dispatcher_task;

    _driver_name = "virtio-net";
    virtio_i("VIRTIO NET INSTANCE");
    _id = _instance++;

    setup_features();
    read_config();

    _hdr_size = (_mergeable_bufs)? sizeof(net_hdr_mrg_rxbuf):sizeof(net_hdr);

    //initialize the BSD interface _if
    _ifn = if_alloc(IFT_ETHER);
    if (_ifn == NULL) {
       //FIXME: need to handle this case - expand the above function not to allocate memory and
       // do it within the constructor.
       net_w("if_alloc failed!");
       return;
    }

    if_initname(_ifn, "eth", _id);
    _ifn->if_mtu = ETHERMTU;
    _ifn->if_softc = static_cast<void*>(this);
    _ifn->if_flags = IFF_BROADCAST /*| IFF_MULTICAST*/;
    _ifn->if_ioctl = if_ioctl;
    _ifn->if_transmit = if_transmit;
    _ifn->if_qflush = if_qflush;
    _ifn->if_init = if_init;
    _ifn->if_getinfo = if_getinfo;
    IFQ_SET_MAXLEN(&_ifn->if_snd, _txq.vqueue->size());

    _ifn->if_capabilities = 0;

    if (_csum) {
        _ifn->if_capabilities |= IFCAP_TXCSUM;
        if (_host_tso4) {
            _ifn->if_capabilities |= IFCAP_TSO4;
            _ifn->if_hwassist = CSUM_TCP | CSUM_UDP | CSUM_TSO;
        }
    }

    if (_guest_csum) {
        _ifn->if_capabilities |= IFCAP_RXCSUM;
        if (_guest_tso4)
            _ifn->if_capabilities |= IFCAP_LRO;
    }

    _ifn->if_capenable = _ifn->if_capabilities | IFCAP_HWSTATS;

    //
    // Enable indirect descriptors utilization.
    // 
    // TODO:
    // Optimize the indirect descriptors infrastructure:
    //  - Preallocate a ring of indirect descriptors per vqueue.
    //  - Consume/recycle from this pool while u can.
    //  - If there is no more free descriptors in the pool above - allocate like
    //    we do today.
    //
    _txq.vqueue->set_use_indirect(true);

    //Start the polling thread before attaching it to the Rx interrupt
    poll_task->start();

    /* TODO: What if_init() is for? */
    tx_dispatcher_task->start();
    // Start with Tx interrupts disabled
    _txq.vqueue->disable_interrupts();

    ether_ifattach(_ifn, _config.mac);
    _msi.easy_register({
        { 0, [&] { _rxq.vqueue->disable_interrupts(); }, poll_task },
        { 1, [&] { _txq.vqueue->disable_interrupts(); }, tx_dispatcher_task }
    });

    fill_rx_ring();

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);
}

net::~net()
{
    //TODO: In theory maintain the list of free instances and gc it
    // including the thread objects and their stack
    // Will need to clear the pending requests in the ring too

    // TODO: add a proper cleanup for a rx.poll_task() here.
    //
    // Since this will involve the rework of the virtio layer - make it for
    // all virtio drivers in a separate patchset.

    ether_ifdetach(_ifn);
    if_free(_ifn);
}

bool net::read_config()
{
    //read all of the net config  in one shot
    virtio_conf_read(virtio_pci_config_offset(), &_config, sizeof(_config));

    if (get_guest_feature_bit(VIRTIO_NET_F_MAC))
        net_i("The mac addr of the device is %x:%x:%x:%x:%x:%x",
                (u32)_config.mac[0],
                (u32)_config.mac[1],
                (u32)_config.mac[2],
                (u32)_config.mac[3],
                (u32)_config.mac[4],
                (u32)_config.mac[5]);

    _mergeable_bufs = get_guest_feature_bit(VIRTIO_NET_F_MRG_RXBUF);
    _status = get_guest_feature_bit(VIRTIO_NET_F_STATUS);
    _tso_ecn = get_guest_feature_bit(VIRTIO_NET_F_GUEST_ECN);
    _host_tso_ecn = get_guest_feature_bit(VIRTIO_NET_F_HOST_ECN);
    _csum = get_guest_feature_bit(VIRTIO_NET_F_CSUM);
    _guest_csum = get_guest_feature_bit(VIRTIO_NET_F_GUEST_CSUM);
    _guest_tso4 = get_guest_feature_bit(VIRTIO_NET_F_GUEST_TSO4);
    _host_tso4 = get_guest_feature_bit(VIRTIO_NET_F_HOST_TSO4);
    _guest_ufo = get_guest_feature_bit(VIRTIO_NET_F_GUEST_UFO);

    net_i("Features: %s=%d,%s=%d", "Status", _status, "TSO_ECN", _tso_ecn);
    net_i("Features: %s=%d,%s=%d", "Host TSO ECN", _host_tso_ecn, "CSUM", _csum);
    net_i("Features: %s=%d,%s=%d", "Guest_csum", _guest_csum, "guest tso4", _guest_tso4);
    net_i("Features: %s=%d", "host tso4", _host_tso4);

    return true;
}

/**
 * Original comment from FreeBSD
 * Alternative method of doing receive checksum offloading. Rather
 * than parsing the received frame down to the IP header, use the
 * csum_offset to determine which CSUM_* flags are appropriate. We
 * can get by with doing this only because the checksum offsets are
 * unique for the things we care about.
 *
 * @return true if csum is bad and false if csum is ok (!!!)
 */
bool net::bad_rx_csum(struct mbuf *m, struct net_hdr *hdr)
{
    struct ether_header *eh;
    struct ether_vlan_header *evh;
    struct udphdr *udp;
    int csum_len;
    u16 eth_type;

    csum_len = hdr->csum_start + hdr->csum_offset;

    if (csum_len < (int)sizeof(struct ether_header) + (int)sizeof(struct ip))
        return true;
    if (m->m_hdr.mh_len < csum_len)
        return true;

    eh = mtod(m, struct ether_header *);
    eth_type = ntohs(eh->ether_type);
    if (eth_type == ETHERTYPE_VLAN) {
        evh = mtod(m, struct ether_vlan_header *);
        eth_type = ntohs(evh->evl_proto);
    }

    // How come - no support for IPv6?!
    if (eth_type != ETHERTYPE_IP) {
        return true;
    }

    /* Use the offset to determine the appropriate CSUM_* flags. */
    switch (hdr->csum_offset) {
    case offsetof(struct udphdr, uh_sum):
        if (m->m_hdr.mh_len < hdr->csum_start + (int)sizeof(struct udphdr))
            return true;
        udp = (struct udphdr *)(mtod(m, uint8_t *) + hdr->csum_start);
        if (udp->uh_sum == 0)
            return false;

        /* FALLTHROUGH */

    case offsetof(struct tcphdr, th_sum):
        m->M_dat.MH.MH_pkthdr.csum_flags |= CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
        m->M_dat.MH.MH_pkthdr.csum_data = 0xFFFF;
        break;

    default:
        return true;
    }

    return false;
}

void net::receiver()
{
    vring* vq = _rxq.vqueue;

    while (1) {

        // Wait for rx queue (used elements)
        virtio_driver::wait_for_queue(vq, &vring::used_ring_not_empty);
        trace_virtio_net_rx_wake();

        u32 len;
        int nbufs;
        struct mbuf* m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
        u32 offset = _hdr_size;
        u64 rx_drops = 0, rx_packets = 0, csum_ok = 0;
        u64 csum_err = 0, rx_bytes = 0;

        // use local header that we copy out of the mbuf since we're
        // truncating it.
        struct net_hdr_mrg_rxbuf mhdr;

        while (m != nullptr) {

            // TODO: should get out of the loop
            vq->get_buf_finalize();

            // Bad packet/buffer - discard and continue to the next one
            if (len < _hdr_size + ETHER_HDR_LEN) {
                rx_drops++;
                m_free(m);

                m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
                continue;
            }

            memcpy(&mhdr, mtod(m, void *), _hdr_size);

            if (!_mergeable_bufs) {
                nbufs = 1;
            } else {
                nbufs = mhdr.num_buffers;
            }

            m->M_dat.MH.MH_pkthdr.len = len;
            m->M_dat.MH.MH_pkthdr.rcvif = _ifn;
            m->M_dat.MH.MH_pkthdr.csum_flags = 0;
            m->m_hdr.mh_len = len;

            struct mbuf* m_head, *m_tail;
            m_tail = m_head = m;

            // Read the fragments
            while (--nbufs > 0) {
                m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
                if (m == nullptr) {
                    rx_drops++;
                    break;
                }

                vq->get_buf_finalize();

                if (m->m_hdr.mh_len < (int)len)
                    len = m->m_hdr.mh_len;

                m->m_hdr.mh_len = len;
                m->m_hdr.mh_flags &= ~M_PKTHDR;
                m_head->M_dat.MH.MH_pkthdr.len += len;
                m_tail->m_hdr.mh_next = m;
                m_tail = m;
            }

            // skip over the virtio header bytes (offset)
            // that aren't need for the above layer
            m_adj(m_head, offset);

            if ((_ifn->if_capenable & IFCAP_RXCSUM) &&
                (mhdr.hdr.flags &
                 net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM)) {
                if (bad_rx_csum(m_head, &mhdr.hdr))
                    csum_err++;
                else
                    csum_ok++;

            }

            rx_packets++;
            rx_bytes += m_head->M_dat.MH.MH_pkthdr.len;

            (*_ifn->if_input)(_ifn, m_head);

            trace_virtio_net_rx_packet(_ifn->if_index, rx_bytes);

            // The interface may have been stopped while we were
            // passing the packet up the network stack.
            if ((_ifn->if_drv_flags & IFF_DRV_RUNNING) == 0)
                break;

            // Move to the next packet
            m = static_cast<struct mbuf*>(vq->get_buf_elem(&len));
        }

        if (vq->refill_ring_cond())
            fill_rx_ring();

        // Update the stats
        _rxq.stats.rx_drops      += rx_drops;
        _rxq.stats.rx_packets    += rx_packets;
        _rxq.stats.rx_csum       += csum_ok;
        _rxq.stats.rx_csum_err   += csum_err;
        _rxq.stats.rx_bytes      += rx_bytes;
    }
}

void net::fill_rx_ring()
{
    trace_virtio_net_fill_rx_ring(_ifn->if_index);
    int added = 0;
    vring* vq = _rxq.vqueue;

    while (vq->avail_ring_not_empty()) {
        struct mbuf *m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, MCLBYTES);
        if (!m)
            break;

        m->m_hdr.mh_len = MCLBYTES;
        u8 *mdata = mtod(m, u8*);

        vq->init_sg();
        vq->add_in_sg(mdata, m->m_hdr.mh_len);
        if (!vq->add_buf(m)) {
            m_freem(m);
            break;
        }
        added++;
    }

    trace_virtio_net_fill_rx_ring_added(_ifn->if_index, added);

    if (added)
        vq->kick();
}

void net::txq::gc()
{
    net_req* req;
    u32 len;
    u16 req_cnt = 0;

    //
    // "finalize" at least every quoter of a ring to let the host work in
    // paralel with us.
    //
    const u16 fin_thr = static_cast<u16>(vqueue->size()) / 4;

    req = static_cast<net_req*>(vqueue->get_buf_elem(&len));

    while(req != nullptr) {
        req->free_mbuf();
        delete req;

        req_cnt++;

        if (req_cnt >= fin_thr) {
            vqueue->get_buf_finalize(req_cnt);
            req_cnt = 0;
        }
        req = static_cast<net_req*>(vqueue->get_buf_elem(&len));
    }

    if (req_cnt) {
        vqueue->get_buf_finalize(req_cnt);
    }

    vqueue->get_buf_gc();
}

int net::txq::try_xmit_one_locked(net_req *req, u64& tx_bytes)
{
    struct mbuf *m, *m_head = req->mb;
    u16 vec_sz = 0;

    DEBUG_ASSERT(!try_lock_running(), "RUNNING lock not taken!\n");
    
    tx_bytes = 0;

    if (m_head->M_dat.MH.MH_pkthdr.csum_flags != 0) {
        m = offload(m_head, &req->mhdr.hdr);
        if ((m_head = m) == nullptr) {
            stats.tx_err++;

            /* The buffer is not well-formed */
            return EINVAL;
        }
    }

    vqueue->init_sg();
    vqueue->add_out_sg(static_cast<void*>(&req->mhdr), _parent->_hdr_size);

    for (m = m_head; m != NULL; m = m->m_hdr.mh_next) {
        int frag_len = m->m_hdr.mh_len;

        if (frag_len != 0) {
            net_d("Frag len=%d:", frag_len);
            req->mhdr.num_buffers++;

            vqueue->add_out_sg(m->m_hdr.mh_data, m->m_hdr.mh_len);
            tx_bytes += frag_len;
        }
    }

    vec_sz = vqueue->_sg_vec.size();

    if (!vqueue->avail_ring_has_room(vec_sz) && vqueue->used_ring_not_empty()) {
        gc();
    }

    if (!vqueue->add_buf(req)) {
        return ENOBUFS;
    }

    return 0;
}

inline void net::txq::update_stats(net_req* req, u64 tx_bytes)
{
    stats.tx_bytes += tx_bytes;
    stats.tx_packets++;

    if (req->mhdr.hdr.flags & net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM)
        stats.tx_csum++;

    if (req->mhdr.hdr.gso_type)
        stats.tx_tso++;
}


int net::txq::xmit_one_locked(mbuf* m_head)
{
    net_req *req = new net_req(m_head);
    int rc = 0;
    u64 tx_bytes = 0;

    //
    // Transmit the packet: don't drop, there is no way to inform the upper
    // layer about this at this stage.
    //
    // In case the packet is a crap there is no other option though.
    //
    rc = try_xmit_one_locked(req, tx_bytes);
    if (rc == EINVAL) {
        req->free_mbuf();
        delete req;

        return rc;
    }

    // We had no HW buffers - wait for completion
    if (rc) {
        // We are going to sleep - kick() the pending packets
        kick();

        do  {
            _parent->virtio_driver::wait_for_queue(vqueue,
                                                   &vring::used_ring_not_empty);
            gc();
        } while (!vqueue->add_buf(req));
    }

    trace_virtio_net_tx_packet(_parent->_ifn->if_index, vqueue->_sg_vec.size());

    // Update the statistics
    update_stats(req, tx_bytes);

    //
    // It was a good packet - increase the counter of a "pending for a kick"
    // packets.
    //
    pkts_to_kick++;

    return 0;
}

mbuf* net::txq::offload(mbuf* m, net_hdr* hdr)
{
    struct ether_header *eh;
    struct ether_vlan_header *evh;
    struct ip *ip;
    struct tcphdr *tcp;
    int ip_offset;
    u16 eth_type, csum_start;
    u8 ip_proto, gso_type = 0;

    ip_offset = sizeof(struct ether_header);
    if (m->m_hdr.mh_len < ip_offset) {
        if ((m = m_pullup(m, ip_offset)) == nullptr)
            return nullptr;
    }

    eh = mtod(m, struct ether_header *);
    eth_type = ntohs(eh->ether_type);
    if (eth_type == ETHERTYPE_VLAN) {
        ip_offset = sizeof(struct ether_vlan_header);
        if (m->m_hdr.mh_len < ip_offset) {
            if ((m = m_pullup(m, ip_offset)) == nullptr)
                return nullptr;
        }
        evh = mtod(m, struct ether_vlan_header *);
        eth_type = ntohs(evh->evl_proto);
    }

    switch (eth_type) {
    case ETHERTYPE_IP:
        if (m->m_hdr.mh_len < ip_offset + (int)sizeof(struct ip)) {
            m = m_pullup(m, ip_offset + sizeof(struct ip));
            if (m == nullptr)
                return nullptr;
        }

        ip = (struct ip *)(mtod(m, uint8_t *) + ip_offset);
        ip_proto = ip->ip_p;
        csum_start = ip_offset + (ip->ip_hl << 2);
        gso_type = net::net_hdr::VIRTIO_NET_HDR_GSO_TCPV4;
        break;

    default:
        return m;
    }

    if (m->M_dat.MH.MH_pkthdr.csum_flags & VIRTIO_NET_CSUM_OFFLOAD) {
        hdr->flags |= net_hdr::VIRTIO_NET_HDR_F_NEEDS_CSUM;
        hdr->csum_start = csum_start;
        hdr->csum_offset = m->M_dat.MH.MH_pkthdr.csum_data;
    }

    if (m->M_dat.MH.MH_pkthdr.csum_flags & CSUM_TSO) {
        if (ip_proto != IPPROTO_TCP)
            return m;

        if (m->m_hdr.mh_len < csum_start + (int)sizeof(struct tcphdr)) {
            m = m_pullup(m, csum_start + sizeof(struct tcphdr));
            if (m == nullptr)
                return nullptr;
        }

        tcp = (struct tcphdr *)(mtod(m, uint8_t *) + csum_start);
        hdr->gso_type = gso_type;
        hdr->hdr_len = csum_start + (tcp->th_off << 2);
        hdr->gso_size = m->M_dat.MH.MH_pkthdr.tso_segsz;

        if (tcp->th_flags & TH_CWR) {
            if (!_parent->_tso_ecn) {
                virtio_w("TSO with ECN not supported by host\n");
                m_freem(m);
                return nullptr;
            }

            hdr->flags |= net_hdr::VIRTIO_NET_HDR_GSO_ECN;
        }
    }

    return m;
}

u32 net::get_driver_features(void)
{
    u32 base = virtio_driver::get_driver_features();
    return (base | (1 << VIRTIO_NET_F_MAC)        \
                 | (1 << VIRTIO_NET_F_MRG_RXBUF)  \
                 | (1 << VIRTIO_NET_F_STATUS)     \
                 | (1 << VIRTIO_NET_F_CSUM)       \
                 | (1 << VIRTIO_NET_F_GUEST_CSUM) \
                 | (1 << VIRTIO_NET_F_GUEST_TSO4) \
                 | (1 << VIRTIO_NET_F_HOST_ECN)   \
                 | (1 << VIRTIO_NET_F_HOST_TSO4)  \
                 | (1 << VIRTIO_NET_F_GUEST_ECN)
                 | (1 << VIRTIO_NET_F_GUEST_UFO)
            );
}

hw_driver* net::probe(hw_device* dev)
{
    return virtio::probe<net, VIRTIO_NET_DEVICE_ID>(dev);
}

} // namespace virtio

