/* guppi_fold_thread.c
 *
 * Fold data, etc.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "fitshead.h"
#include "psrfits.h"
#include "guppi_error.h"
#include "guppi_status.h"
#include "guppi_databuf.h"
#include "polyco.h"
#include "fold.h"

#define STATUS_KEY "FOLDSTAT"
#include "guppi_threads.h"

// Read a status buffer all of the key observation paramters
extern void guppi_read_obs_params(char *buf, 
                                  struct guppi_params *g, 
                                  struct psrfits *p);

/* Parse info from buffer into param struct */
extern void guppi_read_subint_params(char *buf, 
                                     struct guppi_params *g,
                                     struct psrfits *p);


void guppi_fold_thread(void *_args) {

    /* Set priority */
    int rv;
    rv = setpriority(PRIO_PROCESS, 0, 0);
    if (rv<0) {
        guppi_error("guppi_fold_thread", "Error setting priority level.");
        perror("set_priority");
    }

    /* Get arguments */
    struct guppi_thread_args *args = (struct guppi_thread_args *)_args;

    /* Attach to status shared mem area */
    struct guppi_status st;
    rv = guppi_status_attach(&st);
    if (rv!=GUPPI_OK) {
        guppi_error("guppi_fold_thread", 
                "Error attaching to status shared memory.");
        pthread_exit(NULL);
    }
    pthread_cleanup_push((void *)set_exit_status, &st);

    /* Init status */
    guppi_status_lock_safe(&st);
    hputs(st.buf, STATUS_KEY, "init");
    guppi_status_unlock_safe(&st);

    /* Read in general parameters */
    struct guppi_params gp;
    struct psrfits pf;

    /* Attach to databuf shared mem */
    struct guppi_databuf *db_in, *db_out;
    db_in = guppi_databuf_attach(args->input_buffer);
    char errmsg[256];
    if (db_in==NULL) {
        sprintf(errmsg,
                "Error attaching to input databuf(%d) shared memory.", 
                args->input_buffer);
        guppi_error("guppi_fold_thread", errmsg);
        pthread_exit(NULL);
    }
    db_out = guppi_databuf_attach(args->output_buffer);
    if (db_out==NULL) {
        sprintf(errmsg,
                "Error attaching to output databuf(%d) shared memory.", 
                args->output_buffer);
        guppi_error("guppi_fold_thread", errmsg);
        pthread_exit(NULL);
    }

    /* Load polycos */
    int imjd;
    double fmjd, fmjd0, fmjd_next=0.0;
    double tfold=60.0;
    int npc=0, ipc;
    struct polyco *pc=NULL;
    FILE *polyco_file=NULL;

    /* Total foldbuf */
    struct foldbuf fb;
    fb.nbin = 256;
    fb.nchan = 0;
    fb.npol = 0;
    fb.data = NULL;
    fb.count = NULL;

    /* Sub-thread management */
    const int nthread = 4;
    pthread_t thread_id[nthread];
    int input_block_list[nthread];
    struct fold_args fargs[nthread];
    int i;
    for (i=0; i<nthread; i++) {
        thread_id[i] = 0;
        input_block_list[i] = -1;
        fargs[i].data = NULL;
        fargs[i].fb = (struct foldbuf *)malloc(sizeof(struct foldbuf));
        fargs[i].fb->nbin = fb.nbin;
        fargs[i].fb->nchan = 0;
        fargs[i].fb->npol = 0;
        fargs[i].nsamp = 0;
        fargs[i].tsamp = 0.0;
        fargs[i].raw_signed = 1;
    }

    /* Loop */
    int curblock_in=0, curblock_out=0;
    int refresh_polycos=1, next_integration=0, first=1;
    int nblock_int=0, npacket=0, ndrop=0;
    int cur_thread=0;
    char *hdr_in, *hdr_out;
    signal(SIGINT,cc);
    while (run) {

        /* Note waiting status */
        guppi_status_lock_safe(&st);
        hputs(st.buf, STATUS_KEY, "waiting");
        guppi_status_unlock_safe(&st);

        /* Wait for buf to have data */
        guppi_databuf_wait_filled(db_in, curblock_in);

        /* Note waiting status */
        guppi_status_lock_safe(&st);
        hputs(st.buf, STATUS_KEY, "folding");
        guppi_status_unlock_safe(&st);

        /* Read param struct for this block */
        hdr_in = guppi_databuf_header(db_in, curblock_in);
        if (first) 
            guppi_read_obs_params(hdr_in, &gp, &pf);
        else
            guppi_read_subint_params(hdr_in, &gp, &pf);

        /* Refresh params, dump any previous subint on a 0 packet */
        if (gp.packetindex==0)  {
            guppi_read_obs_params(hdr_in, &gp, &pf);
            if (!first) next_integration=1; 
        }

        /* Figure out what time it is */
        imjd = pf.hdr.start_day;
        fmjd = pf.hdr.start_sec 
            + pf.hdr.dt*gp.packetindex*gp.packetsize/pf.hdr.nchan/pf.hdr.npol;
        fmjd /= 86400.0;

        /* Do any first time stuff */
        if (first) {

            /* Set mjds */
            fmjd0 = fmjd;
            fmjd_next = fmjd0 + tfold/86400.0;

            /* Set out fold params */
            fb.nchan = pf.hdr.nchan;
            fb.npol = pf.hdr.npol;

            /* Allocate per-thread foldbufs */
            for (i=0; i<nthread; i++) {
                fargs[i].fb->nbin = fb.nbin;
                fargs[i].fb->nchan = pf.hdr.nchan;
                fargs[i].fb->npol = pf.hdr.npol;
                malloc_foldbuf(fargs[i].fb);
                clear_foldbuf(fargs[i].fb);
            }

            /* Set up first output header */
            hdr_out = guppi_databuf_header(db_out, curblock_out);
            memcpy(hdr_out, guppi_databuf_header(db_in, curblock_in),
                    GUPPI_STATUS_SIZE);
            hputi4(hdr_out, "NBIN", fb.nbin);
                    

            first=0;
        }

        /* Check if we need to move to next subint */
        if (fmjd>fmjd_next) { next_integration=1; }

        /* Combine thread results if needed */
        if (cur_thread==nthread || next_integration) {

            /* Loop over active threads */
            for (i=0; i<cur_thread; i++) {

                /* Wait for thread */
                rv = pthread_join(thread_id[i], NULL);
                if (rv) {
                    guppi_error("guppi_fold_thread", "Error joining subthread");
                    continue;
                }

                /* Mark input block as free */
                if (input_block_list[i]>=0) 
                    guppi_databuf_set_free(db_in, input_block_list[i]);
                
                /* Combine result into total int */
                accumulate_folds(&fb, fargs[i].fb);

                /* Reset thread info */
                clear_foldbuf(fargs[i].fb);
                thread_id[i] = 0;
                input_block_list[i] = -1;
            }

            /* Reset thread count */
            cur_thread = 0;
        }

        /* Finalize this output block if needed, move to next */
        if (next_integration) {

            printf("Finalizing integration\n"); fflush(stdout); // DEBUG

            /* TODO: add any extra info to current output header */

            /* Close out current integration */
            guppi_databuf_set_filled(db_out, curblock_out);

            /* Set up params for next int */
            fmjd0 = fmjd;
            fmjd_next = fmjd0 + tfold/86400.0;
            fb.nchan = pf.hdr.nchan;
            fb.npol = pf.hdr.npol;

            /* Wait for next output buf */
            curblock_out = (curblock_out + 1) % db_out->n_block;
            guppi_databuf_wait_free(db_out, curblock_out);
            hdr_out = guppi_databuf_header(db_out, curblock_out);
            memcpy(hdr_out, guppi_databuf_header(db_in, curblock_in),
                    GUPPI_STATUS_SIZE);
            hputs(hdr_out, "OBS_MODE", "PSR");
            hputi4(hdr_out, "NBIN", fb.nbin);
            hputi4(hdr_out, "PKTIDX", gp.packetindex);

            fb.data = (float *)guppi_databuf_data(db_out, curblock_out);
            fb.count = (unsigned *)(fb.data + fb.nbin * fb.nchan * fb.npol);
            clear_foldbuf(&fb);

            nblock_int=0;
            npacket=0;
            ndrop=0;
            next_integration=0;
        }


        /* Check src, get correct polycos */
        if (refresh_polycos) { 
            polyco_file = fopen("polyco.dat", "r");
            if (polyco_file==NULL) { 
                guppi_error("guppi_fold_thread", "Couldn't open polyco.dat");
                pthread_exit(NULL);
            }
            npc=0;
            do { 
                pc = (struct polyco *)realloc(pc, 
                        sizeof(struct polyco) * (npc+1));
                rv = read_one_pc(polyco_file, &pc[npc]);
                npc++;
            } while (rv==0);
            npc--;
            if (npc==0) { 
                guppi_error("guppi_fold_thread", "Error parsing polyco file.");
                pthread_exit(NULL);
            }
            fclose(polyco_file);
            refresh_polycos=0;
        }

        /* Select polyco set */
        ipc = select_pc(pc, npc, NULL, imjd, fmjd);
        if (ipc<0) { 
            sprintf(errmsg, "No matching polycos (src=%s, imjd=%d, fmjd=%f)",
                    pf.hdr.source, imjd, fmjd);
            guppi_error("guppi_fold_thread", errmsg);
            pthread_exit(NULL);
        }

        /* Launch fold thread */
        input_block_list[cur_thread] = curblock_in;
        fargs[cur_thread].data = guppi_databuf_data(db_in, curblock_in);
        fargs[cur_thread].pc = &pc[ipc];
        fargs[cur_thread].imjd = imjd;
        fargs[cur_thread].fmjd = fmjd;
        fargs[cur_thread].fb->nbin = pf.hdr.nbin;
        fargs[cur_thread].fb->nchan = pf.hdr.nchan;
        fargs[cur_thread].fb->npol = pf.hdr.npol;
        fargs[cur_thread].nsamp = gp.n_packets*gp.packetsize 
            / pf.hdr.nchan / pf.hdr.npol;
        fargs[cur_thread].tsamp = pf.hdr.dt;
        fargs[cur_thread].raw_signed = 1;
        rv = pthread_create(&thread_id[cur_thread], NULL, 
                fold_8bit_power_thread, &fargs[cur_thread]);
        if (rv!=0) 
            guppi_error("guppi_fold_thread", "error launching fold subthread");
        else  
            cur_thread++;

        nblock_int++;
        npacket += gp.n_packets;
        ndrop += gp.n_dropped;
        hputi4(hdr_out, "NBLOCK", nblock_int);
        hputi4(hdr_out, "NPKT", npacket);
        hputi4(hdr_out, "NDROP", ndrop);

        /* Mark in as free.. not yet! */
        //guppi_databuf_set_free(db_in, curblock_in);

        /* Go to next input block */
        curblock_in = (curblock_in + 1) % db_in->n_block;

        /* Check for cancel */
        pthread_testcancel();

    }

    pthread_exit(NULL);

    pthread_cleanup_pop(0); /* Closes set_exit_status */

}
