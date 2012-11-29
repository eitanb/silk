/*
 * Copyight (C) Eitan Ben-Amos, 2012
 *
 * a unit test program to test the silk_kill() API.
 * important cases:
 * call the API from non silk thread, the same silk that is killed or from a different silk instance
 * call the API when some msgs are already queued for the killed instance.
 */

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#define __USE_XOPEN_EXTENDED
#include <unistd.h>
#include "silk.h"


/*
 * the number of silks to dispatch by default
 */
#define DEFAULT_NUM_SILKS          4


/*
 * CLI options
 */
struct options {
    int    num_silk;
} opt;

struct silk_engine_t   engine;



static void
ut_kill__idle_cb(struct silk_execution_thread_t   *exec_thr)
{
    printf("%s\n", __func__);
    sleep(1);
}

/*
 * The different behaviors that are implemented by the Silks we use for the unit-test
*/
enum ut_kill_silk_operation {
    INFINITE_BUSY_WAIT_AND_EXIT,             // test 2
    YIELD_POST_KILL,                         // test 3
    SHALLOW_STACK,                           // test 4
    DEEP_STACK,                              // test 4
};

/*
 * Test 2 control parameters
 */
struct test_2_param_t {
    bool is_busy_wait_started;
    bool is_exit_busy_wait;
} volatile test_2 = {
    .is_busy_wait_started = false,
    .is_exit_busy_wait = false,
};

/*
 * Test 3 control parameters
 */
struct test_3_param_t {
    bool is_busy_waiting;
    bool is_killed;
    bool is_yield_called;
} volatile test_3 = {
    .is_busy_waiting = false,
    .is_killed = false,
    .is_yield_called = false,
};

/*
 * Test 4 control parameters
 */
enum silk_kill_code_path_e {
    SILK_KILL__INVALID = 0,
    SILK_KILL__FIRST,
    /*
     * the silk instance kills itself & then yields.
     */
    SILK_KILL__SUICIDE = SILK_KILL__FIRST,
    /*
     * the silk instance is killed by another instance while it has called yield().
     * The killed instance shouldnt be running even if we send it some msgs.
     */
    SILK_KILL__OTHER_SILK_ON_YEILD,
    /*
     * the silk instance S is killed by another instance while it has already terminated
     * (i.e.: entry fnuction returns) so the silk terminates naturaly.
     */
    SILK_KILL__OTHER_SILK_ON_RET,
    /*
     * the silk instance S is killed by another thread & then yields. S should be recycled
     * and all msgs to it must be discarded.
     */
    SILK_KILL__OTHER_THREAD_AND_YEILD,
    /*
     * the silk instance S is killed by another thread & then returns.
     */
    SILK_KILL__OTHER_THREAD_AND_RET,
#ifdef MULTI_THREAD_ENGINE // only possible with multiple worker thread in engine
    /*
     * the silk instance is killed by another instance & then returns
     * (i.e.: function ends naturaly)
     */
    SILK_KILL__OTHER_SILK_AND_RET,
    /*
     * the silk instance is killed by another instance & then yields
     */
    SILK_KILL__OTHER_SILK_AND_YEILD,
#endif
    SILK_KILL__LAST = SILK_KILL__OTHER_SILK_ON_YEILD//SILK_KILL__OTHER_THREAD_AND_YEILD,

};

struct test_4_param_t {
    // common to all code_path options
    void*    top_stack_frame_addr;
    int      recursion_counter;
    void*    bottom_stack_frame_addr;
    enum silk_kill_code_path_e  code_path;
    // 
    bool     is_killed;
    // other thread killing
    bool     is_busy_waiting;
    bool  has_called_yield;
} volatile test_4 = {
    .code_path = SILK_KILL__INVALID,
};




/*
 * the offset of the first "user" stack frame should be no further than the SHALLOW
 * value of bytes from the stack initial address.
 * The offset of the last "user" stack frame should be at least as far as the DEEP value
 *  of bytes from the stack initial address.
 * Notes:
 * 1) this includes the stack area used by the Silk library before it invokes the "user"
 *    routine.
 * 2) the offsets are obviously dependent on the platform, ABI, atc bcz of different word
 *    size & ABI requirements like:
 * IA32 has a stack frame size aligned on 8 bytes.
 * x86-64 uses many register for parameter passing hence pushing less to stack.
 */
#define TEST_4_RECURSION_DEPTH       1000
#define MAX_SHALLOW_STACK_OFFSET     256
#define MIN_DEEP_STACK_OFFSET        (TEST_4_RECURSION_DEPTH * (8 * 1)) // for IA32



#define SLEEP_INTERVAL 1000   // [usec]


/*
 * wait until the boolean pointed by 'b' has the value of 'exit_value'
 * BEWARE: we use 'volatile' to force gcc to re-read the value from memory in every
 *         iteration. this is not portable to all compilers.
 */
static void
wait_on_bool(volatile bool   *b,
             bool             exit_value)
{
    while (*b != exit_value) {
        usleep(SLEEP_INTERVAL);
    }
}

static void
wait_on_uint32(volatile uint32_t   *val,
               uint32_t             exit_value)
{
    while (*val != exit_value) {
        usleep(SLEEP_INTERVAL);
    }
}

/*
 * calling this API blocks until ALL silks are free.
 */
static void 
wait_for_idle_engine(struct silk_engine_t   *engine)
{
    while (silk_eng__get_free_silks(engine) != engine->cfg.num_silk) {
        usleep(SLEEP_INTERVAL);
    }
}

/*
 * recursively call itself as many times as specifief.
 * the "dummy" variable is used to enlarge the stack frame. note we have to use it in
 * a way that will NOT allow the compiler to optimize it away from the initial stack 
 * frame (e.g.: moving its existence into an inner block level.
 */                         
static void
ut_kill__recurse_n(int    *p_dummy,
                   int     how_many)
{
    int my_dummy = 0;

    if (how_many == 1) {
        test_4.bottom_stack_frame_addr = &my_dummy;
        *p_dummy = time(NULL);
        switch (test_4.code_path) {
        case SILK_KILL__SUICIDE:
            SILK_DEBUG("silk commits suicide");
            silk_kill_id(silk__my_id());
            assert(0); // we shouldnt get here
            break;

        case SILK_KILL__OTHER_SILK_ON_YEILD: {
            struct silk_msg_t  msg;

            test_4.has_called_yield = true;
            SILK_DEBUG("calling yield");
            silk_yield(&msg);
        }
            break;

#ifdef MULTI_THREAD_ENGINE
        case SILK_KILL__OTHER_SILK_AND_RET:
            SILK_DEBUG("I'm now waiting to be killed");
            test_4.is_busy_waiting = true;
            wait_on_bool(&test_4.is_killed, true);
            SILK_DEBUG("i was killed - do clean return");
            break;

        case SILK_KILL__OTHER_SILK_AND_YEILD:
            SILK_DEBUG("I'm now waiting to be killed");
            test_4.is_busy_waiting = true;
            wait_on_bool(&test_4.is_killed, true);
            SILK_DEBUG("i was killed - do yield");
            {
                struct silk_msg_t   msg;
                silk_yield(&msg);
            }
            assert(0); // we shouldnt get here
            break;
#endif

        case SILK_KILL__OTHER_THREAD_AND_RET:
            SILK_DEBUG("I'm now waiting to be killed");
            test_4.is_busy_waiting = true;
            wait_on_bool(&test_4.is_killed, true);
            SILK_DEBUG("i was killed - do clean return");
            break;

        default:
            SILK_DEBUG("Invalid codepath:%d", test_4.code_path);
            assert(0);
        }
    } else {
        ut_kill__recurse_n(&my_dummy, how_many - 1);
    }
}

static void
ut_kill__main(void *_arg)
{
    enum ut_kill_silk_operation   oper = (enum ut_kill_silk_operation)_arg;
    struct silk_t                 *s = silk__my_ctrl();
    struct silk_msg_t             msg;
    //enum silk_status_e  silk_stat;
    int my_dummy = 0;


    SILK_DEBUG("%s: Silk#%d starts... (oper=%d)", __func__, s->silk_id, oper);
    switch (oper){
    case INFINITE_BUSY_WAIT_AND_EXIT:
        SILK_DEBUG("looping indefenitely doing busy wait, waiting for exit signal");
        test_2.is_busy_wait_started = true;
        wait_on_bool(&test_2.is_exit_busy_wait, true);
        SILK_DEBUG("breaking out of busy-wait");
        break;

    case YIELD_POST_KILL:
        SILK_DEBUG("looping indefenitely doing busy wait, waiting for exit signal");
        test_3.is_busy_waiting = true;
        wait_on_bool(&test_3.is_killed, true);
        SILK_DEBUG("breaking out of busy-wait");
        test_3.is_yield_called = true;
        silk_yield(&msg);
        assert(0); // we should not return as we have been killed.
        break;

    case SHALLOW_STACK:
        SILK_DEBUG("Doing a SHALLOW (%d) stack recursion...", test_4.recursion_counter);
        test_4.top_stack_frame_addr = &oper;
        ut_kill__recurse_n(&my_dummy, test_4.recursion_counter);
        break;

    case DEEP_STACK:
        SILK_DEBUG("Doing a DEEP (%d) stack recursion ...", test_4.recursion_counter);
        test_4.top_stack_frame_addr = &oper;
        ut_kill__recurse_n(&my_dummy, test_4.recursion_counter);
        break;

    default:
        assert(0);
    }
    SILK_DEBUG("%s: Silk#%d ends!!!", __func__, s->silk_id);
}

/*
 * a simplest silk-main routine to verify that a silk is running properly
 */
static void
ut_kill__set_val(void *_arg)
{
    volatile uintptr_t   *val = (uintptr_t*)_arg;

    *val = *val + 1234;
}

/*
 * a simple silk entry function to kill another silk
 */
static void
ut_kill__kill_other_silk(void *_arg)
{
    struct silk_t  *s = (struct silk_t*)_arg;
    SILK_DEBUG("Silk#%d killing Silk#%d", silk__my_id(), s->silk_id);
    enum silk_status_e silk_stat = silk_kill(s);
    assert(silk_stat == SILK_STAT_OK);
    test_4.is_killed = true;
}

 __attribute__((noreturn)) static void usage ()
{
    printf("Usage: ut_kill <num silks>\n");
    exit(EINVAL);
}

int main (int   argc, char **argv)
{
    struct silk_engine_param_t    silk_cfg = {
        .flags = 0,
        // it's possible to select an address or let the library choose
        //.stack_addr = (void*)NULL,
        .stack_addr = (void*)0xb0000000,
        .num_stack_pages = 16,
        .num_stack_seperator_pages = 4,
        .num_silk = DEFAULT_NUM_SILKS,
        .idle_cb = ut_kill__idle_cb,
        .ctx = NULL,
    };
    struct silk_t          *s, *s2;
#define UT_KILL__MAGIC_1  0x12345678
#define UT_KILL__MAGIC_2  0xfedcba98
    uintptr_t              arg;
    enum silk_status_e     silk_stat;
    int    rep;


    // set CLI defaults
    opt.num_silk = DEFAULT_NUM_SILKS;

    // read CLI options 
    if (argc > 1) {
        if (argc >= 2) {
            opt.num_silk = atoi(argv[1]);
            if (opt.num_silk < 1) {
                usage();
            }
        }
    }

    SILK_DEBUG("Initializing Silk engine...");
    silk_cfg.num_silk = opt.num_silk;
    silk_stat = silk_init(&engine, &silk_cfg);
    SILK_DEBUG("Silk initialization returns:%d", silk_stat);


    // Test 1 : terminate a non dispacthed silk
    printf("Test Case 1\n");
    silk_stat = silk_alloc(&engine, ut_kill__main, (void*)NULL, &s);
    assert(silk_stat == SILK_STAT_OK);
    SILK_DEBUG("allocated silk No %d", s->silk_id);
    assert(engine.num_free_silk == opt.num_silk - 1);
    silk_stat = silk_eng_kill(&engine, s);
    assert(silk_stat == SILK_STAT_OK);
    // we expect all silks to be free now
    assert(engine.num_free_silk == opt.num_silk);
    assert(SILK_STATE(s) == SILK_STATE__FREE);

    // Test 2: terminate a dispatched silk that ends without calling yield.
    printf("Test Case 2\n");
    silk_stat = silk_alloc(&engine, ut_kill__main, (void*)INFINITE_BUSY_WAIT_AND_EXIT, &s);
    assert(silk_stat == SILK_STAT_OK);
    SILK_DEBUG("allocated silk No %d", s->silk_id);
    silk_stat = silk_dispatch(&engine, s);
    assert(silk_stat == SILK_STAT_OK);
    SILK_DEBUG("dispatched silk#%d to do busy-wait", s->silk_id);
    SILK_DEBUG("waiting for the silk to enter busy-wait");
    wait_on_bool(&test_2.is_busy_wait_started, true);
    SILK_DEBUG("requesting silk#%d to exit", s->silk_id);
    test_2.is_exit_busy_wait = true;
    SILK_DEBUG("wait for the silk to become free");
    // wait for the silk to be recycled
    while (SILK_STATE(s) != SILK_STATE__FREE) {
        usleep(SLEEP_INTERVAL);
    }
    assert(engine.num_free_silk == opt.num_silk);
    silk_stat = silk_eng_kill(&engine, s);
    assert(silk_stat == SILK_STAT_OK);

    /*
     * Test 3: terminate a dispatched silk that yields after we killed it
     */
    printf("Test Case 3\n");
    silk_stat = silk_alloc(&engine, ut_kill__main, (void*)YIELD_POST_KILL, &s);
    assert(silk_stat == SILK_STAT_OK);
    SILK_DEBUG("allocated silk No %d", s->silk_id);
    silk_stat = silk_dispatch(&engine, s);
    assert(silk_stat == SILK_STAT_OK);
    SILK_DEBUG("dispatched silk#%d to wait for kill", s->silk_id);
    SILK_DEBUG("waiting for the silk to enter busy-wait");
    wait_on_bool(&test_3.is_busy_waiting, true);
    silk_stat = silk_eng_kill(&engine, s);
    assert(silk_stat == SILK_STAT_OK);
    test_3.is_killed = true;
    SILK_DEBUG("wait for silk to call yield()");
    wait_on_bool(&test_3.is_yield_called, true);
    SILK_DEBUG("wait for the silk to become free");
    // wait for the silk to be recycled
    while (SILK_STATE(s) != SILK_STATE__FREE) {
        usleep(SLEEP_INTERVAL);
    }
    SILK_DEBUG("Silk#%d is free", s->silk_id);
    assert(engine.num_free_silk == opt.num_silk);

    SILK_DEBUG("Now allocate a silk, make sure its the same instance we killed,"
               " dispatch it to see it running fine.");
    arg = UT_KILL__MAGIC_1;
    silk_stat = silk_alloc(&engine, ut_kill__set_val, (void*)&arg, &s2);
    assert(silk_stat == SILK_STAT_OK);
    assert(s2->silk_id == s->silk_id);
    silk_stat = silk_dispatch(&engine, s2);
    assert(silk_stat == SILK_STAT_OK);
    wait_on_uint32(&arg, UT_KILL__MAGIC_1 + 1234);
    assert(engine.num_free_silk == opt.num_silk);


    /*
     * Test 4: allocate the same silk instance again & again. the instance will do a
     * shalow run & then a highly recursive run. noting that every stack frame is a
     * multiple of 8 bytes, w ecan have a recursion depth of 100 to make sure we move
     * deep into the stack.
     * on both cases, well take the address of the initial stack frame & verify its 
     * the same.
     * we'll do the whole thing a 100 times to make sure we dont keep slipping within
     * the stack area.
     * This checks that:
     * 1) we can still properly use the same silk instance we used & killed before.
     * 2) killing a silk properly resets its stack state so it starts with all stack 
     *    area available.
     * 3) we kill silks with deep recursion to make sure we dont slip within the stack area.
     * 4) we kill from all possible callers:
     *    a) the silk itself (SILK_KILL__SUICIDE_YIELD).
     *    b) another silk we dispatch just to do the kill
     *    c) the external thread.
     * 5) we also compare it to silks which call yield() (for recycling) or just wrap-up
     *    the stack & return nicely
     * as there are 5 combinations, we'll use a stride of 5 steps, such that each stride
     * repeats all 5 cases
     */
    uintptr_t   start_stk_addr, end_stk_addr;
    uintptr_t   stk_used;
    silk_id_t   expected_silk_id = s->silk_id;
    void        *prev_top = NULL, *prev_bottom = NULL;


    printf("Test Case 4\n");
    end_stk_addr = (uintptr_t)silk_get_stack_from_id(&engine, expected_silk_id);
    start_stk_addr = end_stk_addr + SILK_USEABLE_STACK(&engine.cfg);
    SILK_DEBUG("Stack range for Silk#%d:start_stk_addr=%p, end_stk_addr=%p", 
               expected_silk_id, (void*)start_stk_addr, (void*)end_stk_addr);
    for (rep = 0; rep < 10; rep++) {
        for (test_4.code_path=SILK_KILL__FIRST;
             test_4.code_path<=SILK_KILL__LAST; 
             test_4.code_path++) {
            SILK_DEBUG("-----------------iteration#%d, path=%d------------------------",
                       rep, test_4.code_path);
            // posion output info
            test_4.top_stack_frame_addr = 0;
            test_4.recursion_counter = 1;
            test_4.bottom_stack_frame_addr = 0;
            // reset signaling variables.
            test_4.is_busy_waiting = false;
            test_4.is_killed = false;
            // verify the codepath we test now
            assert((test_4.code_path >= SILK_KILL__FIRST) && (test_4.code_path <= SILK_KILL__LAST));
            SILK_DEBUG("using codepath # %d", test_4.code_path);
            // dipatch the shallow silk & wait for its termination
            silk_stat = silk_alloc(&engine, ut_kill__main, (void*)SHALLOW_STACK, &s);
            assert(silk_stat == SILK_STAT_OK);
            assert(s->silk_id == expected_silk_id);
            silk_stat = silk_dispatch(&engine, s);
            assert(silk_stat == SILK_STAT_OK);
            test_4.is_killed = false;
            switch (test_4.code_path) {
            case SILK_KILL__SUICIDE:
                break;

            case SILK_KILL__OTHER_SILK_ON_YEILD:
                wait_on_bool(&test_4.has_called_yield, true);
                silk_stat = silk_alloc(&engine, ut_kill__kill_other_silk, s, &s2);
                assert(silk_stat == SILK_STAT_OK);
                SILK_DEBUG("start another silk (#%d) to kill the first one", s2->silk_id);
                assert(s2->silk_id != expected_silk_id);//toggle == & !=
                silk_stat = silk_dispatch(&engine, s2);
                assert(silk_stat == SILK_STAT_OK);
                wait_on_bool(&test_4.is_killed, true);
                SILK_DEBUG("Silk#%d killed Silk#%d", s2->silk_id, s->silk_id);
                break;

#ifdef MULTI_THREAD_ENGINE
            case SILK_KILL__OTHER_SILK_AND_RET:
                wait_on_bool(&test_4.is_busy_waiting, true);
                silk_stat = silk_alloc(&engine, ut_kill__kill_other_silk, s, &s2);
                assert(silk_stat == SILK_STAT_OK);
                SILK_DEBUG("start another silk (#%d) to kill the first one", s2->silk_id);
                assert(s2->silk_id != expected_silk_id);
                silk_stat = silk_dispatch(&engine, s2);
                assert(silk_stat == SILK_STAT_OK);
                break;
#endif

            case SILK_KILL__OTHER_THREAD_AND_RET:
                wait_on_bool(&test_4.is_busy_waiting, true);
                silk_stat = silk_eng_kill(&engine, s);
                assert(silk_stat == SILK_STAT_OK);
                test_4.is_killed = true;
                break;

            default:
                SILK_DEBUG("Invalid codepath:%d", test_4.code_path);
                assert(0);
            } // switch
            wait_for_idle_engine(&engine);
            assert(engine.num_free_silk == opt.num_silk);
            SILK_DEBUG("SHALLOW: top_stack_frame_addr=%p, bottom_stack_frame_addr=%p",
                       test_4.top_stack_frame_addr, test_4.bottom_stack_frame_addr);
            // remember x86 stack grows downward in addresses
            stk_used = start_stk_addr - (uintptr_t)test_4.top_stack_frame_addr;
            SILK_DEBUG("stack being used by SHALLOW silk code:0x%x Bytes", stk_used);
            assert(stk_used < MAX_SHALLOW_STACK_OFFSET);

            // dipatch the deep silk & wait for its termination
            silk_stat = silk_alloc(&engine, ut_kill__main, (void*)DEEP_STACK, &s);
            assert(silk_stat == SILK_STAT_OK);
            assert(s->silk_id == expected_silk_id);
            test_4.recursion_counter = TEST_4_RECURSION_DEPTH;
            silk_stat = silk_dispatch(&engine, s);
            assert(silk_stat == SILK_STAT_OK);
            wait_for_idle_engine(&engine);
            assert(engine.num_free_silk == opt.num_silk);
            SILK_DEBUG("DEEP: top_stack_frame_addr=%p, bottom_stack_frame_addr=%p",
                       test_4.top_stack_frame_addr, test_4.bottom_stack_frame_addr);
            // remember x86 stack grows downward in addresses
            stk_used = start_stk_addr - (uintptr_t)test_4.bottom_stack_frame_addr;
            SILK_DEBUG("stack being used by DEEP silk code:0x%x Bytes", stk_used);
            assert(stk_used > MIN_DEEP_STACK_OFFSET);

            if (rep > 0) {
                SILK_DEBUG("verifying stack addresses are identical to previous run");
                assert(prev_top == test_4.top_stack_frame_addr);
                assert(prev_bottom == test_4.bottom_stack_frame_addr);
            }
            // save stack addresses for comparison in next test iteration
            prev_top = test_4.top_stack_frame_addr;
            prev_bottom = test_4.bottom_stack_frame_addr;
        // TODO: kill the silk in the DEEP recursion so we know its usage was deep
        }
    }

    // request engine to terminate
    silk_stat = silk_terminate(&engine);
    SILK_DEBUG("Silk termination returns:%d", silk_stat);

    silk_stat = silk_join(&engine);
    SILK_DEBUG("Silk join returns:%d", silk_stat);
}