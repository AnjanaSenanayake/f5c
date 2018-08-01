#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f5c.h"
#include "f5cmisc.h"

#define m_min_mapping_quality 30

core_t* init_core(const char* bamfilename, const char* fastafile,
                  const char* fastqfile, opt_t opt) {
    core_t* core = (core_t*)malloc(sizeof(core_t));
    MALLOC_CHK(core);

    // load bam file
    core->m_bam_fh = sam_open(bamfilename, "r");
    NULL_CHK(core->m_bam_fh);

    // load bam index file
    core->m_bam_idx = sam_index_load(core->m_bam_fh, bamfilename);
    NULL_CHK(core->m_bam_idx);

    // read the bam header
    core->m_hdr = sam_hdr_read(core->m_bam_fh);
    NULL_CHK(core->m_hdr);

    core->itr = sam_itr_queryi(core->m_bam_idx, HTS_IDX_START, 0, 0);
    NULL_CHK(core->itr);

    // reference file
    core->fai = fai_load(fastafile);
    NULL_CHK(core->fai);

    // readbb
    core->readbb = ReadDB_alloc();
    ReadDB_load(core->readbb, fastqfile);

    //model
    core->model = (model_t*)malloc(sizeof(model_t) *
                                   4096); //4096 is 4^6 which os hardcoded now
    MALLOC_CHK(core->model);
    //load the model from files

    core->opt = opt;
    return core;
}

void free_core(core_t* core) {
    free(core->model);
    ReadDB_free(core->readbb);
    fai_destroy(core->fai);
    sam_itr_destroy(core->itr);
    bam_hdr_destroy(core->m_hdr);
    hts_idx_destroy(core->m_bam_idx);
    sam_close(core->m_bam_fh);
    free(core);
}

db_t* init_db() {
    db_t* db = (db_t*)(malloc(sizeof(db_t)));
    MALLOC_CHK(db);

    db->capacity_bam_rec = 512;
    db->n_bam_rec = 0;

    db->bam_rec = (bam1_t**)(malloc(sizeof(bam1_t*) * db->capacity_bam_rec));
    MALLOC_CHK(db->bam_rec);

    int32_t i = 0;
    for (i = 0; i < db->capacity_bam_rec; ++i) {
        db->bam_rec[i] = bam_init1();
        NULL_CHK(db->bam_rec[i]);
    }

    db->fasta_cache = (char**)(malloc(sizeof(char*) * db->capacity_bam_rec));
    MALLOC_CHK(db->fasta_cache);
    db->f5 = (fast5_t**)malloc(sizeof(fast5_t*) * db->capacity_bam_rec);
    MALLOC_CHK(db->f5);

    db->et = (event_table*)malloc(sizeof(event_table) * db->capacity_bam_rec);
    MALLOC_CHK(db->et);

    return db;
}

int32_t load_db(core_t* core, db_t* db) {
    // get bams
    bam1_t* record;
    int32_t result = 0;
    db->n_bam_rec = 0;
    int32_t i = 0;
    while (db->n_bam_rec < db->capacity_bam_rec) {
        record = db->bam_rec[db->n_bam_rec];
        result = sam_itr_next(core->m_bam_fh, core->itr, record);

        if (result < 0) {
            break;
        } else {
            if ((record->core.flag & BAM_FUNMAP) == 0 &&
                record->core.qual >=
                    m_min_mapping_quality) { // remove secondraies? //need to use the user parameter
                // printf("%s\t%d\n",bam_get_qname(db->bam_rec[db->n_bam_rec]),result);
                db->n_bam_rec++;
            }
        }
    }
    // fprintf(stderr,"%s:: %d queries read\n",__func__,db->n_bam_rec);

    // get ref sequences (can make efficient by taking the the start and end of
    // the sorted bam)
    for (i = 0; i < db->n_bam_rec; i++) {
        bam1_t* record = db->bam_rec[i];
        char* ref_name = core->m_hdr->target_name[record->core.tid];
        // printf("refname : %s\n",ref_name);
        int32_t ref_start_pos = record->core.pos;
        int32_t ref_end_pos = bam_endpos(record);
        assert(ref_end_pos >= ref_start_pos);

        // Extract the reference sequence for this region
        int32_t fetched_len = 0;
        char* refseq =
            faidx_fetch_seq(core->fai, ref_name, ref_start_pos, ref_end_pos,
                            &fetched_len); // error handle?
        db->fasta_cache[i] = refseq;
        // printf("seq : %s\n",db->fasta_cache[i]);

        // get the fast5

        // Get the read type from the fast5 file
        // TODO: Free the two strings below? Also strlen is inefficent
        const char* qname = bam_get_qname(db->bam_rec[i]);
        const char* path = ReadDB_get_signal_path(core->readbb, qname);
        int len = strlen(path) + 10;

        char* fast5_path = (char*)malloc(len + 10);
        MALLOC_CHK(fast5_path);
        strcpy(fast5_path, qname);

        hid_t hdf5_file = fast5_open(fast5_path);
        if (hdf5_file >= 0) {
            db->f5[i] = (fast5_t*)calloc(1, sizeof(fast5_t));
            MALLOC_CHK(db->f5[i]);
            fast5_read(hdf5_file, db->f5[i]); // todo : errorhandle
            fast5_close(hdf5_file);
        } else {
            WARNING("Fast5 file is unreadable and will be skipped: %s",
                    fast5_path);
        }

        if (core->opt.print_raw) {
            printf("@%s\t%s\t%llu\n", qname, fast5_path,
                   db->f5[i]->nsample);
            uint32_t j = 0;
            for (j = 0; j < db->f5[i]->nsample; j++) {
                printf("%d\t", (int)db->f5[i]->rawptr[j]);
            }
            printf("\n");
        }

        free(fast5_path);
    }
    // fprintf(stderr,"%s:: %d fast5 read\n",__func__,db->n_bam_rec);

    return db->n_bam_rec;
}

void process_db(core_t* core, db_t* db) {
    int32_t i;
    for (i = 0; i < db->n_bam_rec; i++) {
        float* rawptr = db->f5[i]->rawptr;
        float range = db->f5[i]->range;
        float digitisation = db->f5[i]->digitisation;
        float offset = db->f5[i]->offset;
        int32_t nsample = db->f5[i]->nsample;

        // convert to pA
        float raw_unit = range / digitisation;
        for (int32_t j = 0; j < nsample; j++) {
            rawptr[j] = (rawptr[j] + offset) * raw_unit;
        }
        db->et[i] = getevents(db->f5[i]->nsample, rawptr);

        //have to test if the computed events are correct

        //then we should be ready to directly call adaptive_banded_simple_event_align
    }

    return;
}

void free_db_tmp(db_t* db) {
    int32_t i = 0;
    for (i = 0; i < db->n_bam_rec; ++i) {
        bam_destroy1(db->bam_rec[i]);
        db->bam_rec[i] = bam_init1();
        free(db->fasta_cache[i]);
        free(db->f5[i]->rawptr);
        free(db->f5[i]);
        free(db->et[i].event);
    }
}

void free_db(db_t* db) {
    int32_t i = 0;
    for (i = 0; i < db->capacity_bam_rec; ++i) {
        bam_destroy1(db->bam_rec[i]);
    }
    free(db->bam_rec);
    free(db->fasta_cache);
    free(db->et);
    free(db->f5);
    free(db);
}

void init_opt(opt_t* opt) {
    memset(opt, 0, sizeof(opt_t));

    opt->print_raw = 0;
    opt->min_mapq = 30;
    opt->con_sec = 0;
}
