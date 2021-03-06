/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

// verify that two transactions doing cursor next and prev operations detect a deadlock when
// using write locking cursor operations.

#include "test.h"
#include "toku_pthread.h"

static uint64_t get_key(DBT *key) {
    uint64_t k = 0;
    assert(key->size == sizeof k);
    memcpy(&k, key->data, key->size);
    return htonl(k);
}

static void populate(DB_ENV *db_env, DB *db, uint64_t nrows) {
    int r;

    DB_TXN *txn = NULL;
    r = db_env->txn_begin(db_env, NULL, &txn, 0); assert(r == 0);

    for (uint64_t i = 0; i < nrows; i++) {

        uint64_t k = htonl(i);
        uint64_t v = i;
        DBT key = { .data = &k, .size = sizeof k };
        DBT val = { .data = &v, .size = sizeof v };
        r = db->put(db, txn, &key, &val, 0); assert(r == 0);
    }

    r = txn->commit(txn, 0); assert(r == 0);
}

struct my_callback_context {
    DBT key;
    DBT val;
};

static int blocking_next_callback(DBT const *a UU(), DBT const *b UU(), void *e UU()) {
    DBT const *found_key = a;
    DBT const *found_val = b;
    struct my_callback_context *context = (struct my_callback_context *) e;
    copy_dbt(&context->key, found_key);
    copy_dbt(&context->val, found_val);
    return 0;
}

static void blocking_next(DB_ENV *db_env, DB *db, uint64_t nrows UU(), long sleeptime) {
    int r;

    struct my_callback_context context;
    dbt_init_realloc(&context.key);
    dbt_init_realloc(&context.val);

    DB_TXN *txn = NULL;
    r = db_env->txn_begin(db_env, NULL, &txn, 0); assert(r == 0);

    DBC *cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0); assert(r == 0);

    uint64_t i;
    for (i = 0; ; i++) {
        r = cursor->c_getf_next(cursor, DB_RMW, blocking_next_callback, &context);
        if (r != 0)
            break;
        if (verbose)
            printf("%lu next %" PRIu64 "\n", (unsigned long) toku_pthread_self(), get_key(&context.key));
        usleep(sleeptime);
    }

    if (verbose)
        printf("%lu next=%d\n", (unsigned long) toku_pthread_self(), r);
    assert(r == DB_NOTFOUND || r == DB_LOCK_DEADLOCK);

    int rr = cursor->c_close(cursor); assert(rr == 0);

    if (r == DB_NOTFOUND) {
        if (verbose) printf("%lu commit\n", (unsigned long) toku_pthread_self());
        r = txn->commit(txn, 0); 
    } else {
        if (verbose) printf("%lu abort\n", (unsigned long) toku_pthread_self());
        r = txn->abort(txn);
    }
    assert(r == 0);

    toku_free(context.key.data);
    toku_free(context.val.data);
}

static void blocking_prev(DB_ENV *db_env, DB *db, uint64_t nrows UU(), long sleeptime) {
    int r;

    struct my_callback_context context;
    dbt_init_realloc(&context.key);
    dbt_init_realloc(&context.val);

    DB_TXN *txn = NULL;
    r = db_env->txn_begin(db_env, NULL, &txn, 0); assert(r == 0);

    DBC *cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0); assert(r == 0);

    uint64_t i;
    for (i = 0; ; i++) {
        r = cursor->c_getf_prev(cursor, DB_RMW, blocking_next_callback, &context);
        if (r != 0)
            break;
        if (verbose)
            printf("%lu prev %" PRIu64 "\n", (unsigned long) toku_pthread_self(), get_key(&context.key));
        usleep(sleeptime);
    }

    if (verbose)
        printf("%lu prev=%d\n", (unsigned long) toku_pthread_self(), r);
    assert(r == DB_NOTFOUND || r == DB_LOCK_DEADLOCK);

    int rr = cursor->c_close(cursor); assert(rr == 0);

    if (r == DB_NOTFOUND) {
        if (verbose) printf("%lu commit\n", (unsigned long) toku_pthread_self());
        r = txn->commit(txn, 0); 
    } else {
        if (verbose) printf("%lu abort\n", (unsigned long) toku_pthread_self());
        r = txn->abort(txn);
    }
    assert(r == 0);

    toku_free(context.key.data);
    toku_free(context.val.data);
}

struct blocking_next_args {
    DB_ENV *db_env;
    DB *db;
    uint64_t nrows;
    long sleeptime;
};

static void *blocking_next_thread(void *arg) {
    struct blocking_next_args *a = (struct blocking_next_args *) arg;
    blocking_next(a->db_env, a->db, a->nrows, a->sleeptime);
    return arg;
}

static void run_test(DB_ENV *db_env, DB *db, int nthreads, uint64_t nrows, long sleeptime) {
    int r;
    toku_pthread_t tids[nthreads];
    struct blocking_next_args a = {db_env, db, nrows, sleeptime};
    for (int i = 0; i < nthreads - 1; i++) {
        r = toku_pthread_create(
            toku_uninstrumented, &tids[i], nullptr, blocking_next_thread, &a);
        assert(r == 0);
    }
    blocking_prev(db_env, db, nrows, sleeptime);
    for (int i = 0; i < nthreads - 1; i++) {
        void *ret;
        r = toku_pthread_join(tids[i], &ret); assert(r == 0);
    }
}

int test_main(int argc, char * const argv[]) {
    uint64_t cachesize = 0;
    uint32_t pagesize = 0;
    uint64_t nrows = 10;
    int nthreads = 2;
    long sleeptime = 100000;
    const char *db_env_dir = TOKU_TEST_FILENAME;
    const char *db_filename = "test.db";
    int db_env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_INIT_TXN | DB_INIT_LOCK | DB_INIT_LOG | DB_THREAD;

    // parse_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            if (verbose > 0)
                verbose--;
            continue;
        }
        if (strcmp(argv[i], "--nrows") == 0 && i+1 < argc) {
            nrows = atoll(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--nthreads") == 0 && i+1 < argc) {
            nthreads = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--sleeptime") == 0 && i+1 < argc) {
            sleeptime = atol(argv[++i]);
            continue;
        }
        assert(0);
    }

    // setup env
    int r;
    char rm_cmd[strlen(db_env_dir) + strlen("rm -rf ") + 1];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", db_env_dir);
    r = system(rm_cmd); assert(r == 0);

    r = toku_os_mkdir(db_env_dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH); assert(r == 0);

    DB_ENV *db_env = NULL;
    r = db_env_create(&db_env, 0); assert(r == 0);
    if (cachesize) {
        const uint64_t gig = 1 << 30;
        r = db_env->set_cachesize(db_env, cachesize / gig, cachesize % gig, 1); assert(r == 0);
    }
    r = db_env->open(db_env, db_env_dir, db_env_open_flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); assert(r == 0);
    r = db_env->set_lock_timeout(db_env, 30 * 1000, nullptr); assert(r == 0);

    // create the db
    DB *db = NULL;
    r = db_create(&db, db_env, 0); assert(r == 0);
    if (pagesize) {
        r = db->set_pagesize(db, pagesize); assert(r == 0);
    }
    r = db->open(db, NULL, db_filename, NULL, DB_BTREE, DB_CREATE|DB_AUTO_COMMIT|DB_THREAD, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); assert(r == 0);

    // populate the db
    populate(db_env, db, nrows);

    run_test(db_env, db, nthreads, nrows, sleeptime);

    // close env
    r = db->close(db, 0); assert(r == 0); db = NULL;
    r = db_env->close(db_env, 0); assert(r == 0); db_env = NULL;

    return 0;
}
