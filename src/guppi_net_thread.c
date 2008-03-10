/* guppi_net_thread.c
 *
 * Routine to read packets from network and put them
 * into shared memory blocks.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "fitshead.h"
#include "guppi_error.h"
#include "guppi_status.h"
#include "guppi_databuf.h"
#include "guppi_udp.h"

/* This thread is passed a single arg, pointer
 * to the guppi_udp_params struct.  This thread should 
 * be cancelled and restarted if any hardware params
 * change, as this potentially affects packet size, etc.
 */
void *guppi_net_thread(void *_up) {

    /* Set cpu affinity */
    cpu_set_t cpuset, cpuset_orig;
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset_orig);
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    int rv = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    if (rv<0) { 
        guppi_error("guppi_net_thread", "Error setting cpu affinity.");
        perror("sched_setaffinity");
    }

    /* Set priority */
    rv = setpriority(PRIO_PROCESS, 0, 0);
    if (rv<0) {
        guppi_error("guppi_net_thread", "Error setting priority level.");
        perror("set_priority");
    }

    /* Get UDP param struct */
    struct guppi_udp_params *up =
        (struct guppi_udp_params *)_up;

    /* Attach to status shared mem area */
    struct guppi_status st;
    rv = guppi_status_attach(&st);
    if (rv!=GUPPI_OK) {
        guppi_error("guppi_net_thread", 
                "Error attaching to status shared memory.");
        pthread_exit(NULL);
    }

    /* Read in general parameters */
    struct guppi_params gp;
    pthread_cleanup_push((void *)guppi_status_unlock, &st);
    guppi_status_lock(&st);
    guppi_read_params(st.buf, &gp);
    guppi_status_unlock(&st);
    pthread_cleanup_pop(0);

    /* Attach to databuf shared mem */
    struct guppi_databuf *db;
    db = guppi_databuf_attach(1); // TODO don't hardcode this 1
    if (db==NULL) {
        guppi_error("guppi_net_thread",
                "Error attaching to databuf shared memory.");
        pthread_exit(NULL);
    }

    /* Set up UDP socket */
    rv = guppi_udp_init(up);
    if (rv!=GUPPI_OK) {
        guppi_error("guppi_net_thread",
                "Error opening UDP socket.");
        pthread_exit(NULL);
    }
    pthread_cleanup_push((void *)guppi_udp_close, up);

    /* Figure out size of data in each packet, number of packets
     * per block, etc.
     * TODO : Figure out how/if to deal with packet size changing.
     */
    struct guppi_udp_packet p;
    size_t packet_hdr_size = (char *)p.data - (char *)(&p);
    size_t packet_data_size = up->packet_size - packet_hdr_size;
    unsigned packets_per_block = db->block_size / packet_data_size;

    /* Counters */
    unsigned long long npacket_total=0, npacket_block=0;
    unsigned long long ndropped_total=0, ndropped_block=0;
    unsigned long long nbogus_total=0, nbogus_block=0;
    unsigned long long curblock_seq_num=0, nextblock_seq_num=0;
    unsigned long long last_seq_num=2048;
    int curblock=-1;
    char *curheader=NULL, *curdata=NULL;
    unsigned block_packet_idx=0, last_block_packet_idx=0;
    double drop_frac_avg=0.0;
    const double drop_lpf = 0.25;

    /* Main loop */
    unsigned i, force_new_block=0, waiting=0;
    char *dataptr;
    long long seq_num_diff;
    while (1) {

        /* Wait for data */
        rv = guppi_udp_wait(up);
        if (rv!=GUPPI_OK) {
            if (rv==GUPPI_TIMEOUT) { 
                /* Set "waiting" flag */
                if (!waiting) {
                    guppi_status_lock(&st);
                    hputs(st.buf, "NETSTAT", "waiting");
                    guppi_status_unlock(&st);
                    waiting=1;
                }
                continue; 
            } else {
                guppi_error("guppi_net_thread", 
                        "guppi_udp_wait returned error");
                perror("guppi_udp_wait");
                pthread_exit(NULL);
            }
        }

        /* Read packet */
        rv = guppi_udp_recv(up, &p);
        if (rv!=GUPPI_OK) {
            if (rv==GUPPI_ERR_PACKET) {
                /* Unexpected packet size, ignore? */
                nbogus_total++;
                nbogus_block++;
                continue; 
            } else {
                guppi_error("guppi_net_thread", 
                        "guppi_udp_recv returned error");
                perror("guppi_udp_recv");
                pthread_exit(NULL);
            }
        }

        /* Update status if needed */
        if (waiting) {
            guppi_status_lock(&st);
            hputs(st.buf, "NETSTAT", "receiving");
            guppi_status_unlock(&st);
            waiting=0;
        }

        /* Check seq num diff */
        seq_num_diff = p.seq_num - last_seq_num;
        if (seq_num_diff<0) { 
            if (seq_num_diff<-1024) { force_new_block=1; }
            else  { continue; } /* No going backwards */
        } else { force_new_block=0; }

        /* Determine if we go to next block */
        if ((p.seq_num>=nextblock_seq_num) || force_new_block) {

            if (curblock>=0) { 
                /* Close out current block */
                hputi4(curheader, "PKTIDX", curblock_seq_num);
                hputi4(curheader, "PKTSIZE", packet_data_size);
                hputi4(curheader, "NPKT", npacket_block);
                hputi4(curheader, "NDROP", ndropped_block);
                guppi_write_params(curheader, &gp);
                guppi_databuf_set_filled(db, curblock);
            }

            if (npacket_block) { 
                drop_frac_avg = (1.0-drop_lpf)*drop_frac_avg 
                    + drop_lpf*(double)ndropped_block/(double)npacket_block;
            }

            /* Put drop stats in general status area */
            hputr8(st.buf, "DROPAVG", drop_frac_avg);
            hputr8(st.buf, "DROPTOT", 
                    npacket_total ? 
                    (double)ndropped_total/(double)npacket_total 
                    : 0.0);
            hputr8(st.buf, "DROPBLK", 
                    npacket_block ? 
                    (double)ndropped_block/(double)npacket_block 
                    : 0.0);

            /* Reset block counters */
            npacket_block=0;
            ndropped_block=0;
            nbogus_block=0;

            /* Advance to next one */
            curblock = (curblock + 1) % db->n_block;
            curheader = guppi_databuf_header(db, curblock);
            curdata = guppi_databuf_data(db, curblock);
            last_block_packet_idx = 0;
            curblock_seq_num = p.seq_num - (p.seq_num % packets_per_block);
            nextblock_seq_num = curblock_seq_num + packets_per_block;
            guppi_databuf_wait_free(db, curblock);
        }

        /* Skip dropped blocks, put packet in right spot */
        block_packet_idx = p.seq_num - curblock_seq_num;
        dataptr = curdata + last_block_packet_idx*packet_data_size;
        for (i=last_block_packet_idx; i<block_packet_idx; i++) {
            memset(dataptr, 0, packet_data_size);
            dataptr += packet_data_size;
            ndropped_block++;
            ndropped_total++;
            npacket_total++;
            npacket_block++;
        }
        memcpy(dataptr, p.data, packet_data_size);
        npacket_total++;
        npacket_block++;
        last_block_packet_idx = block_packet_idx + 1;
        last_seq_num = p.seq_num;

        /* Will exit if thread has been cancelled */
        pthread_testcancel();
    }

    /* Have to close all push's */
    pthread_cleanup_pop(0); /* Closes push(guppi_udp_close) */
}
