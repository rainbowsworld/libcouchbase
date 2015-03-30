#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libcouchbase/couchbase.h>
#include <libcouchbase/api3.h>
#include <unistd.h>

#define VALUE_SIZE 1048576
static int counter = 0;
static const char *key = "Hello";
static char value[VALUE_SIZE];

static void store_cb(lcb_t instance, int cbtype, const lcb_RESPSTORE *resp)
{
    assert(resp->rc == LCB_SUCCESS);
    counter--;
    printf("-");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    lcb_t instance;
    lcb_error_t rc;
    int ii;
    struct lcb_create_st options = { 0 };
    lcb_CMDSTORE cmd = { 0 };

    if (argc != 2) {
        fprintf(stderr, "Must have connection string!\n");
        exit(EXIT_FAILURE);
    }

    options.version = 3;
    options.v.v3.connstr = argv[1];

    rc = lcb_create(&instance, &options);
    assert(rc == LCB_SUCCESS);

    rc = lcb_cntl_string(instance, "operation_timeout", "120");
    assert(rc == LCB_SUCCESS);

    rc = lcb_connect(instance);
    assert(rc == LCB_SUCCESS);

    lcb_wait(instance);
    rc = lcb_get_bootstrap_status(instance);
    assert(rc == LCB_SUCCESS);

    lcb_install_callback3(instance, LCB_CALLBACK_STORE, (lcb_RESPCALLBACK)store_cb);

    // fill the value so valgrind doesn't warn about unitialized buffers
    for (ii = 0; ii < VALUE_SIZE; ii++) {
        value[ii] = '*';
    }

    LCB_CMD_SET_KEY(&cmd, key, strlen(key));
    LCB_CMD_SET_VALUE(&cmd, value, VALUE_SIZE);
    cmd.operation = LCB_SET;

    printf("Running sample. This will schedule 1000 operations, invoking \n");
    printf("an event loop tick after each one. The tick is non-blocking\n");
    printf("It will sleep 500 microseconds between each operation to allow\n");
    printf("for the asynchronous sending of the buffer's contents to the\n");
    printf("server.\n\n");
    printf("LEGEND:\n");
    printf("  + => Operation Scheduled\n");
    printf("  - => Operation Completed\n");

    for (ii = 0; ii < 1000; ii++) {
        lcb_sched_enter(instance);

        // Note: lcb_store() implicitly does lcb_sched_enter(), lcb_store3(),
        // and lcb_sched_leave().
        rc = lcb_store3(instance, NULL, &cmd);
        assert(rc == LCB_SUCCESS);
        lcb_sched_leave(instance);
        counter++;

        // This is like lcb_wait(), except it does not block.
        lcb_tick_nowait(instance);

        // Sleep to demonstrate.. Naturally the longer the wait time, the
        // clearer the difference between the tick and non-tick versions
        usleep(100);
        printf("+");
        fflush(stdout);
    }

    printf("\nCalling lcb_wait()\n");
    lcb_wait(instance);
    printf("\n");
    lcb_destroy(instance);
}

/**
 * Sample output
 * +++++++++++++--+----+----+-++++++++++++++-++----------+++-+--+-------++++++++--+------++-+++++++++-+++++-++++-+++++-+++++-++++-+++++-++++-++++++++++-++-++++-++++-++++-++++++-+++++-+++++-+++++-+++++-+++++-++++++++---++-++-++-+-+++-+-+++-+-+++-+-++-+-+-+++-+++-+-++-++-++-+--+-+-++-+++-+--+-+-++-+-+++++++++++++++-+-+-+-+--++-++++++++++++-++++-+++++++++-++++-++-++++++-+++++-+++-+++++-+++-+++++-++++-++++--++-++-++-+-++++++++-+---+++--++-+-+-+++--+-+--+-++++--++--+-+-+-+++++-+-+--++-+++-+-+--+--+-+--+++-+-++-+--+-++++-+--++++++++--+-++---++-++-++---+-+--+-++++--+++--+-+-+--+-+++-++++++---+------------+-----------------------------------------------------------++++----------------------------------------------------------------------------------------------------++++-+++++++--+-+--+-+++++-+--+--+--++++-+-++--+-+-+--++++-+--+++-+-+--+-+--+-++++++++-+----++-+-------------------------+------------------------------++++++++++-+-+++-+-+--+-+---++-+-++++-+-+-+-+--+++-+--------+------++---++++-+++-+-+-+-+--+++-+++-+--+-+-+++-+-++-+-+-+--+-++----+---+------+++++++--+++-+++++-------+-++++++---------+++++-+---+-+-+++-++-----+-----+++++-+-+-++++++--+-+-+--+-+-+-++++--+-+--+-+++++--+--+-++-+----+-----++++++---++++-+++++-++++++-++++-++++-+++++-++++++++-+++++-+++++-+++++-++++-++++-+++++-+++++-+++++-+++++-++++-++++-++++++-+++++-++++-+++-++++-+++++-++++-+++-++++++-+++-++-+++-+--++-++-++-+--+++++----+++-+--+--+-+--++-+++-+-+--+--++-+++-+--+-++++--+-+-+-++-+++-+--+-+--+-+-++++--+-+--+-+-+-+++-+--+-+--+-++-+-++-+-+-+-+-+--+-++-+-+-------+-----+------+------------------+--------------------+------------------------------------------------------+---++++++++-++++-+++++++-++-+++++-+++++-++++-+++++-++++++-++++-+++-++-+-+-+++-++-+-+++-+-++-+++++++++------++-++-+--+-+--++++++++-----+-+++--+-+-+--+-+-+++-++-+-+-+--+-++-++++-+--+--+-+-+++-+++-+--+-+--+-++-+------+--------------+------------------------------------------------------+-------+++++++++-++++++-+++-++++-++++++-+
 * Calling lcb_wait()
 * ----------------------------
 *
 */
