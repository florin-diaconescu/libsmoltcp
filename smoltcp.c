#include <uk/essentials.h>
/* Import user configuration: */
#include <uk/config.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <uk/netdev.h>
#include <uk/alloc.h>
#include <uk/init.h>
#include <assert.h>
#include <string.h>

#include "smoltcp.h"

#define UKNETDEV_BPS 1000000000u
#define UKNETDEV_BUFLEN 2048

#define ETH_PAD_SIZE 2

static uint16_t tx_headroom = ETH_PAD_SIZE;
static uint16_t rx_headroom = ETH_PAD_SIZE;

struct uk_netdev *dev;
struct uk_netbuf *netbuf;

/* These headers are taken from linux */
struct	ether_header {
	uint8_t	ether_dhost[6];
	uint8_t	ether_shost[6];
	uint16_t ether_type;
}__attribute__((packed));

struct udphdr {
	uint16_t source;
	uint16_t dest;
	uint16_t len;
	uint16_t check;
}__attribute__((packed));

struct iphdr 
{
	uint8_t	ihl:4,
		version:4;
	uint8_t	tos;
	uint16_t	tot_len;
	uint16_t	id;
	uint16_t	frag_off;
	uint8_t	ttl;
	uint8_t	protocol;
	uint16_t	check;
	uint32_t	saddr;
	uint32_t	daddr;
}__attribute__((packed));

struct uk_netbuf *alloc_netbuf(struct uk_alloc *a, size_t alloc_size,
		size_t headroom)
{
	void *allocation;
	struct uk_netbuf *b;

	allocation = uk_malloc(a, alloc_size);
	if (unlikely(!allocation))
		goto err_out;

	b = uk_netbuf_prepare_buf(allocation, alloc_size,
			headroom, 0, NULL);
	if (unlikely(!b)) {
		goto err_free_allocation;
	}

	b->_a = a;
	b->len = b->buflen - headroom;

	return b;

err_free_allocation:
	uk_free(a, allocation);
err_out:
	return NULL;
}

static uint16_t netif_alloc_rxpkts(void *argp, struct uk_netbuf *nb[],
		uint16_t count)
{
	struct uk_alloc *a;
	uint16_t i;

	UK_ASSERT(argp);

	a = (struct uk_alloc *) argp;

	for (i = 0; i < count; ++i) {
		nb[i] = alloc_netbuf(a, UKNETDEV_BUFLEN, rx_headroom);
		assert(nb[i]);
	}

	return i;
}

static inline struct PacketInfo packet_handler(struct uk_netdev *dev,
		uint16_t queue_id __unused, void *argp)
{
	int ret;
	struct uk_netbuf *nb;
	struct PacketInfo pi;

back:
    ret = uk_netdev_rx_one(dev, 0, &nb);

    if (uk_netdev_status_notready(ret)) {
        goto back;
    }

    netbuf = nb;

    pi.packet = nb->data;
    pi.size = nb->len;

	return pi;
}

struct PacketInfo packet_handler_wrapper() {
    struct PacketInfo pi;
	pi = packet_handler(dev, 0, NULL);

	return pi;
}

static inline void uknetdev_output(struct uk_netdev *dev, struct uk_netbuf *nb)
{
	int ret;

	do {
		ret = uk_netdev_tx_one(dev, 0, nb);
	} while(uk_netdev_status_notready(ret));

	if (ret < 0) {
		uk_netbuf_free_single(nb);
	}
}

void uknetdev_output_wrapper (struct PacketInfo packet) {
    struct uk_alloc *a = uk_alloc_get_default();
    assert(a != NULL);

    struct uk_netbuf *nb = alloc_netbuf(a, 148, tx_headroom);
    assert (nb != NULL);

    memcpy(nb->data, packet.packet, packet.size);

	uknetdev_output(dev, nb);
}

static int libsmoltcp_init() {
    struct uk_alloc *a;

	struct uk_netdev_conf dev_conf;
	struct uk_netdev_rxqueue_conf rxq_conf;
	struct uk_netdev_txqueue_conf txq_conf;
	int devid = 0;
	int ret;

	/* Get pointer to default UK allocator */
	a = uk_alloc_get_default();
	assert(a != NULL);

	dev = uk_netdev_get(devid);
	assert(dev != NULL);

	struct uk_netdev_info info;
	uk_netdev_info_get(dev, &info);
	assert(info.max_tx_queues);
	assert(info.max_rx_queues);

	assert(uk_netdev_state_get(dev) == UK_NETDEV_UNCONFIGURED);

	rx_headroom = (rx_headroom < info.nb_encap_rx)
		? info.nb_encap_rx : rx_headroom;
	tx_headroom = (tx_headroom < info.nb_encap_tx)
		? info.nb_encap_tx : tx_headroom;

	dev_conf.nb_rx_queues = 1;
	dev_conf.nb_tx_queues = 1;

	/* Configure the device */
	ret = uk_netdev_configure(dev, &dev_conf);
	assert(ret >= 0);

	/* Configure the RX queue */
	rxq_conf.a = a;
	rxq_conf.alloc_rxpkts = netif_alloc_rxpkts;
	rxq_conf.alloc_rxpkts_argp = a;

	//printf("Running busy waiting\n");
	rxq_conf.callback = NULL;
	rxq_conf.callback_cookie = NULL;

	ret = uk_netdev_rxq_configure(dev, 0, 0, &rxq_conf);
	assert(ret >= 0);

	/*  Configure the TX queue*/
	txq_conf.a = a;
	ret = uk_netdev_txq_configure(dev, 0, 0, &txq_conf);
	assert(ret >= 0);

	/* GET mTU */
	uint16_t mtu = uk_netdev_mtu_get(dev);
	assert(mtu == 1500);

	/* Start the netdev */
	ret = uk_netdev_start(dev);

	/* No interrupts */
	ret = uk_netdev_rxq_intr_disable(dev, 0);
	assert(ret >= 0);

    return 0;
}

uk_lib_initcall(libsmoltcp_init);