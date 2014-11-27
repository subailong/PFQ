/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/vmalloc.h>
#include <linux/printk.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/pf_q.h>

#include <pf_q-mpdb-queue.h>
#include <pf_q-bitops.h>
#include <pf_q-module.h>
#include <pf_q-sock.h>
#include <pf_q-global.h>
#include <pf_q-memory.h>
#include <pf_q-GC.h>


static inline
void *pfq_skb_copy_from_linear_data(const struct sk_buff *skb, void *to, size_t len)
{
	if (len < 64 && (len + skb_tailroom(skb) >= 64))
		return memcpy(to, skb->data, 64);
	return memcpy(to, skb->data, len);
}


static inline
char *mpdb_slot_ptr(struct pfq_rx_opt *ro, struct pfq_rx_queue_hdr *qd, size_t index, size_t slot)
{
	return (char *)(ro->queue_base) + ( (index&1 ? ro->size : 0 ) + slot) * ro->slot_size;
}


size_t pfq_mpdb_enqueue_batch(struct pfq_rx_opt *ro,
		              struct gc_queue_buff *queue,
		              unsigned long long mask,
		              int burst_len,
		              int gid)
{
	struct pfq_rx_queue_hdr *rx_queue = pfq_get_rx_queue_hdr(ro);
	int data, qlen, qindex;
	struct gc_buff buff;
	size_t n, sent = 0;
	char *this_slot;

	if (unlikely(rx_queue == NULL))
		return 0;

	data = atomic_read((atomic_t *)&rx_queue->data);

        if (MPDB_QUEUE_LEN(data) > ro->size)
		return 0;

	data = atomic_add_return(burst_len, (atomic_t *)&rx_queue->data);

	qlen      = MPDB_QUEUE_LEN(data) - burst_len;
	qindex    = MPDB_QUEUE_INDEX(data);
        this_slot = mpdb_slot_ptr(ro, rx_queue, qindex, qlen);

	for_each_gcbuff_bitmask(queue, mask, buff, n)
	{
		volatile struct pfq_pkt_hdr *hdr;
		struct sk_buff *skb = buff.skb;
		size_t bytes, slot_index;
		char *pkt;

		bytes = min((size_t)skb->len, ro->caplen);
		slot_index = qlen + sent;

		hdr = (struct pfq_pkt_hdr *)this_slot;
		pkt = (char *)(hdr+1);

		if (slot_index > ro->size) {

			if (waitqueue_active(&ro->waitqueue)) {
#ifdef PFQ_USE_EXTENDED_PROC
 				sparse_inc(&global_stats.wake);
#endif
				wake_up_interruptible(&ro->waitqueue);
			}

			return sent;
		}

		/* copy bytes of packet */

#ifdef PFQ_USE_SKB_LINEARIZE
		if (unlikely(skb_is_nonlinear(skb)))
#else
		if (skb_is_nonlinear(skb))
#endif
		{
			if (skb_copy_bits(skb, 0, pkt, bytes) != 0) {
				printk(KERN_WARNING "[PFQ] BUG! skb_copy_bits failed (bytes=%zu, skb_len=%d mac_len=%d)!\n",
							    bytes, skb->len, skb->mac_len);
				return 0;
			}
		}
		else {
			pfq_skb_copy_from_linear_data(skb, pkt, bytes);
		}

                /* copy mark from pfq_cb (annotation) */

                hdr->data = PFQ_CB(skb)->mark;

		/* setup the header */

		if (ro->tstamp != 0) {
			struct timespec ts;
			skb_get_timestampns(skb, &ts);
			hdr->tstamp.tv.sec  = (uint32_t)ts.tv_sec;
			hdr->tstamp.tv.nsec = (uint32_t)ts.tv_nsec;
		}

		hdr->if_index    = skb->dev->ifindex & 0xff;
		hdr->gid         = gid;

		hdr->len         = (uint16_t)skb->len;
		hdr->caplen 	 = (uint16_t)bytes;
		hdr->un.vlan_tci = skb->vlan_tci & ~VLAN_TAG_PRESENT;
		hdr->hw_queue    = (uint8_t)(skb_get_rx_queue(skb) & 0xff);

		/* commit the slot (release semantic) */

		smp_wmb();

		hdr->commit = (uint8_t)qindex;

		if ((slot_index & 8191) == 0 &&
				waitqueue_active(&ro->waitqueue)) {
#ifdef PFQ_USE_EXTENDED_PROC
 			sparse_inc(&global_stats.wake);
#endif
		        wake_up_interruptible(&ro->waitqueue);
		}

		sent++;

		this_slot += ro->slot_size;
	}

	return sent;
}


int pfq_mpdb_shared_queue_alloc(struct pfq_sock *so, size_t queue_mem)
{
        /* calculate the size of the buffer */

	size_t tm = PAGE_ALIGN(queue_mem);
        size_t tot_mem;

	/* align bufflen to page size */

	size_t num_pages = tm / PAGE_SIZE; void *addr;

	num_pages += (num_pages + (PAGE_SIZE-1)) & (PAGE_SIZE-1);
	tot_mem = num_pages*PAGE_SIZE;

	/* Memory is already zeroed */

        addr = vmalloc_user(tot_mem);
	if (addr == NULL) {
		printk(KERN_WARNING "[PFQ|%d] pfq_queue_alloc: out of memory (vmalloc %zu bytes)!", so->id, tot_mem);
		return -ENOMEM;
	}

        so->mem_addr = addr;
        so->mem_size = tot_mem;

	pr_devel("[PFQ|%d] pfq_queue_alloc: caplen=%zu maxlen=%zu memory=%zu bytes.\n", so->id, so->rx_opt.caplen, so->tx_opt.maxlen, tot_mem);
	return 0;
}


void pfq_mpdb_shared_queue_free(struct pfq_sock *so)
{
	if (so->mem_addr) {

		vfree(so->mem_addr);

		so->mem_addr = NULL;
		so->mem_size = 0;

		pr_devel("[PFQ|%d] queue freed.\n", so->id);
	}
}


int pfq_mpdb_shared_queue_toggle(struct pfq_sock *so, bool active)
{
        if (active)
        {
                if (!so->mem_addr) {

                        struct pfq_queue_hdr * queue;

                        /* alloc queue memory */

                        if (pfq_mpdb_shared_queue_alloc(so, pfq_queue_total_mem(so)) < 0)
                        {
                                return -ENOMEM;
                        }

                        /* so->mem_addr and so->mem_size are correctly configured */

                        /* initialize queues headers */

                        queue = (struct pfq_queue_hdr *)so->mem_addr;

                        /* initialize rx queue header */

                        queue->rx.data              = (1L << 24);
                        queue->rx.size              = so->rx_opt.size;
                        queue->rx.slot_size         = so->rx_opt.slot_size;

                        queue->tx.producer.index    = 0;
                        queue->tx.producer.cache    = 0;
                        queue->tx.consumer.index    = 0;
                        queue->tx.consumer.cache    = 0;

                        queue->tx.size_mask         = so->tx_opt.size - 1;
                        queue->tx.max_len           = so->tx_opt.maxlen;
                        queue->tx.size              = so->tx_opt.size;
                        queue->tx.slot_size         = so->tx_opt.slot_size;

                        /* update the queues base_addr */

                        so->rx_opt.queue_base = so->mem_addr + sizeof(struct pfq_queue_hdr);
                        so->tx_opt.queue_base = so->mem_addr + sizeof(struct pfq_queue_hdr) + pfq_queue_mpdb_mem(so);

                        /* commit both the queues */

                        smp_wmb();

			atomic_long_set(&so->rx_opt.queue_hdr, (long)&queue->rx);
			atomic_long_set(&so->tx_opt.queue_hdr, (long)&queue->tx);

                        pr_devel("[PFQ|%d] tx/rx queues enabled: rx_size=%d rx_slot_size=%d tx_size=%d tx_slot_size=%d\n", so->id, queue->rx.size,
                                        queue->rx.slot_size,
                                        queue->tx.size,
                                        queue->tx.slot_size);
                }
        }
        else
        {
                if (so->mem_addr) {

			atomic_long_set(&so->rx_opt.queue_hdr, 0);
			atomic_long_set(&so->tx_opt.queue_hdr, 0);

                        msleep(Q_GRACE_PERIOD);

                        pfq_mpdb_shared_queue_free(so);

			pr_devel("[PFQ|%d] tx/rx queues disabled.\n", so->id);
                }
        }

        return 0;
}
