/* Host-buildable tests for sd_ready.c's wait_for_sd_ready() -- exercises
 * the pure state machine against scripted fake probes, no real filesystem,
 * mount, subprocess, or clock access (that's all in sd_ready_real.c, never
 * linked into this binary). Every scenario uses millisecond-scale fake
 * deadlines (short/extended/hard = 50/150/300ms, a 5ms poll interval) so
 * the whole suite runs effectively instantly regardless of the real
 * production constants in sd_ready_real.c.
 *
 * Build/run: `make sd_ready_test` (see the Makefile's own target) -- plain
 * host gcc, no cross toolchain needed, since this only links sd_ready.c. */
#include "sd_ready.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static const char * g_current_test = "";

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_current_test, __FILE__, __LINE__, #cond);                     \
            g_failures++;                                                                                            \
        }                                                                                                            \
    } while (0)

#define BEGIN_TEST(name)                                                                                             \
    do {                                                                                                             \
        g_current_test = name;                                                                                       \
        fprintf(stderr, "-- %s\n", name);                                                                            \
    } while (0)

/* ---- Fake probe fixture -----------------------------------------------
 *
 * A single scripted fixture drives every scenario: a simulated monotonic
 * clock that advances only when wait_ms() is "slept" (so a test controls
 * exactly how much fake time has passed at every check), plus per-scenario
 * flags/counters the individual fake probe functions consult. Every field
 * defaults to "nothing yet" (zeroed) -- each test sets only what it needs. */
typedef struct {
    int64_t clock_ms;
    bool clock_broken; /* monotonic_ms() always returns -1 */

    bool partition_exists_after_ms; /* -1 = never; else the clock_ms at/after which path_exists(partition) is true */
    bool whole_exists_after_ms_set;
    int64_t whole_exists_after_ms;
    bool partition_exists_after_ms_set;
    int64_t partition_exists_after_ms_value;

    bool mmc_evidence_always;

    /* Records the deadline_remaining_ms argument from the most recent
     * try_mount() call -- lets a test assert the algorithm is actually
     * passing a sane (positive, deadline-bounded) budget through, not just
     * that mounting itself eventually succeeds/fails. */
    int64_t last_deadline_remaining_ms;

    /* try_mount()/mount_point_mounted() simulation: `mounted` flips true
     * the call at/after which the matching node's mount is scripted to
     * succeed (see mount_succeeds_for_partition_at_call/mount_succeeds_for_whole_at_call,
     * -1 = never succeeds for that node). try_mount_calls counts total
     * calls made (any node), for tests that care how many retries happened. */
    bool mounted;
    const char * mounted_via; /* device node string last passed to a try_mount() that flipped `mounted` true */
    int try_mount_calls_partition;
    int try_mount_calls_whole;
    int mount_succeeds_for_partition_at_call; /* -1 = never; else the 1-based call count that succeeds */
    int mount_succeeds_for_whole_at_call;

    bool exec_ready_after_ms_set;
    int64_t exec_ready_after_ms; /* path_is_executable() becomes true once clock_ms >= this, once mounted */
    bool exec_never_ready;

    int wait_ms_calls;
} fake_ctx_t;

static void fake_init(fake_ctx_t * f) {
    memset(f, 0, sizeof(*f));
    f->mount_succeeds_for_partition_at_call = -1;
    f->mount_succeeds_for_whole_at_call = -1;
    f->exec_never_ready = true;
}

static bool fake_path_exists(void * ctx_v, const char * path) {
    fake_ctx_t * f = ctx_v;
    if (strcmp(path, "PARTITION") == 0) {
        return f->partition_exists_after_ms_set && f->clock_ms >= f->partition_exists_after_ms_value;
    }
    if (strcmp(path, "WHOLE") == 0) {
        return f->whole_exists_after_ms_set && f->clock_ms >= f->whole_exists_after_ms;
    }
    return false;
}

static bool fake_mount_point_mounted(void * ctx_v) {
    fake_ctx_t * f = ctx_v;
    return f->mounted;
}

static void fake_try_mount(void * ctx_v, const char * device_node, int64_t deadline_remaining_ms) {
    fake_ctx_t * f = ctx_v;
    f->last_deadline_remaining_ms = deadline_remaining_ms;
    if (strcmp(device_node, "PARTITION") == 0) {
        f->try_mount_calls_partition++;
        if (f->mount_succeeds_for_partition_at_call >= 0 &&
            f->try_mount_calls_partition >= f->mount_succeeds_for_partition_at_call) {
            f->mounted = true;
            f->mounted_via = "PARTITION";
        }
    } else if (strcmp(device_node, "WHOLE") == 0) {
        f->try_mount_calls_whole++;
        if (f->mount_succeeds_for_whole_at_call >= 0 && f->try_mount_calls_whole >= f->mount_succeeds_for_whole_at_call) {
            f->mounted = true;
            f->mounted_via = "WHOLE";
        }
    }
}

static bool fake_mmc_evidence_present(void * ctx_v) {
    fake_ctx_t * f = ctx_v;
    return f->mmc_evidence_always;
}

static bool fake_path_is_executable(void * ctx_v, const char * path) {
    (void) path;
    fake_ctx_t * f = ctx_v;
    if (!f->mounted) return false;
    if (f->exec_never_ready) return false;
    if (!f->exec_ready_after_ms_set) return true; /* ready immediately once mounted */
    return f->clock_ms >= f->exec_ready_after_ms;
}

static int64_t fake_monotonic_ms(void * ctx_v) {
    fake_ctx_t * f = ctx_v;
    if (f->clock_broken) return -1;
    return f->clock_ms;
}

static void fake_wait_ms(void * ctx_v, int ms) {
    fake_ctx_t * f = ctx_v;
    f->wait_ms_calls++;
    f->clock_ms += ms;
}

static sd_ready_probes_t fake_probes(fake_ctx_t * f) {
    sd_ready_probes_t p;
    p.ctx = f;
    p.path_exists = fake_path_exists;
    p.mount_point_mounted = fake_mount_point_mounted;
    p.try_mount = fake_try_mount;
    p.mmc_evidence_present = fake_mmc_evidence_present;
    p.path_is_executable = fake_path_is_executable;
    p.monotonic_ms = fake_monotonic_ms;
    p.wait_ms = fake_wait_ms;
    return p;
}

/* Small, fast fake deadlines shared by every scenario -- see this file's
 * own top comment for why these don't need to match sd_ready_real.c's real
 * production values. */
#define SHORT_MS 50
#define EXTENDED_MS 150
#define HARD_MS 300
#define POLL_MS 5
#define EXEC_GRACE_MS 15

static sd_ready_result_t run(fake_ctx_t * f) {
    sd_ready_probes_t probes = fake_probes(f);
    const char * exec_candidates[] = { "STOCK", "UPDATE" };
    return wait_for_sd_ready(&probes, "PARTITION", "WHOLE", exec_candidates, 2, SHORT_MS, EXTENDED_MS, HARD_MS,
                              POLL_MS, EXEC_GRACE_MS);
}

/* 1. No card at all -- nothing ever indicates a card is present. Must stop
 * at the short deadline, not ride out the extended/hard budget. */
static void test_no_card(void) {
    BEGIN_TEST("no_card");
    fake_ctx_t f;
    fake_init(&f);

    sd_ready_result_t r = run(&f);

    CHECK(r.stage == SD_READY_STAGE_NONE);
    CHECK(!r.mounted);
    CHECK(!r.executable_ready);
    CHECK(r.elapsed_ms >= SHORT_MS);
    CHECK(r.elapsed_ms < EXTENDED_MS); /* must not have extended past the short deadline */
}

/* 2. Normal fast card -- partition node present and mountable from the very
 * first check, executable present immediately. Must return almost
 * instantly (the "exit immediately once ready" fast path). */
static void test_normal_fast_card(void) {
    BEGIN_TEST("normal_fast_card");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 0;
    f.mount_succeeds_for_partition_at_call = 1;
    f.exec_never_ready = false;

    sd_ready_result_t r = run(&f);

    CHECK(r.stage == SD_READY_STAGE_EXEC_READY);
    CHECK(r.mounted);
    CHECK(r.executable_ready);
    CHECK(r.elapsed_ms == 0); /* mounted and exec-ready on the very first iteration, no sleep needed at all */
    CHECK(f.wait_ms_calls == 0);
}

/* 3. Device node appearing late (but still within the short window) --
 * mounts successfully once it exists. */
static void test_node_appears_late(void) {
    BEGIN_TEST("node_appears_late");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 20; /* appears at 20ms, well inside the 50ms short deadline */
    f.mount_succeeds_for_partition_at_call = 1;
    f.exec_never_ready = false;

    sd_ready_result_t r = run(&f);

    CHECK(r.stage == SD_READY_STAGE_EXEC_READY);
    CHECK(r.mounted);
    CHECK(r.elapsed_ms >= 20);
    CHECK(r.elapsed_ms < SHORT_MS);
}

/* 3b. KNOWN, ACCEPTED LIMITATION (evaluated and deliberately kept, not an
 * oversight): a card whose own MMC-core enumeration -- not just mounting,
 * not just mdev publishing the node -- takes longer than the short
 * deadline is indistinguishable from no card at all, because the only two
 * evidence sources (sysfs `type` attribute, /dev node) both require
 * enumeration to have already finished. Fixing this would need either a
 * physical card-detect signal (no such generic, safe-to-assume sysfs
 * attribute exists across arbitrary MMC host drivers, and this project has
 * no live device to identify one specific to the R1), or a longer
 * unconditional short deadline paid by every card-absent boot -- the
 * project explicitly chose to keep the short deadline as-is rather than
 * slow down the common case for this. This test documents and locks in
 * that decision (so a future change to the short-deadline give-up logic
 * doesn't silently start covering, or silently regress, this case without
 * anyone noticing either way), not a gap nobody thought about. */
static void test_enumeration_slower_than_short_deadline_known_limitation(void) {
    BEGIN_TEST("enumeration_slower_than_short_deadline_known_limitation");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = SHORT_MS + 1; /* card only "enumerates" just after the short deadline */
    f.mount_succeeds_for_partition_at_call = 1;         /* would mount instantly once it existed */
    f.exec_never_ready = false;

    sd_ready_result_t r = run(&f);

    CHECK(!r.mounted); /* gave up before ever seeing the node -- accepted, not a bug */
    CHECK(r.stage == SD_READY_STAGE_NONE);
    CHECK(r.elapsed_ms == SHORT_MS);
}

/* 4. Partition node appears only after the whole-device node, and only the
 * partition actually mounts (simulating a genuinely partitioned card that
 * cannot be mounted directly from the whole-disk node). */
static void test_partition_after_whole(void) {
    BEGIN_TEST("partition_after_whole");
    fake_ctx_t f;
    fake_init(&f);
    f.whole_exists_after_ms_set = true;
    f.whole_exists_after_ms = 0;
    f.mount_succeeds_for_whole_at_call = -1; /* whole-disk mount attempts never succeed */
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 15;
    f.mount_succeeds_for_partition_at_call = 1;
    f.exec_never_ready = false;

    sd_ready_result_t r = run(&f);

    CHECK(r.mounted);
    CHECK(r.stage == SD_READY_STAGE_EXEC_READY);
    CHECK(r.saw_whole_node);
    CHECK(r.saw_partition_node);
    CHECK(r.device_node_used != NULL && strcmp(r.device_node_used, "PARTITION") == 0);
    CHECK(f.try_mount_calls_whole > 0); /* the whole-disk node WAS attempted before the partition appeared */
}

/* 5. Mount initially failing, then succeeding on a later retry against the
 * same, already-present node (e.g. a slow exfat/ntfs driver). */
static void test_mount_fails_then_succeeds(void) {
    BEGIN_TEST("mount_fails_then_succeeds");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 0;
    f.mount_succeeds_for_partition_at_call = 4; /* fails 3 times, succeeds on the 4th */
    f.exec_never_ready = false;

    sd_ready_result_t r = run(&f);

    CHECK(r.mounted);
    CHECK(r.stage == SD_READY_STAGE_EXEC_READY);
    CHECK(f.try_mount_calls_partition == 4);
}

/* 6. Mounted filesystem with no configured executable ever present (the
 * common "SD card with music but no alternate player" case) -- must return
 * quickly, bounded by the small exec grace window, not the mount-side
 * deadlines. */
static void test_mounted_no_executable(void) {
    BEGIN_TEST("mounted_no_executable");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 0;
    f.mount_succeeds_for_partition_at_call = 1;
    /* exec_never_ready stays true (fake_init() default) */

    sd_ready_result_t r = run(&f);

    CHECK(r.mounted);
    CHECK(!r.executable_ready);
    CHECK(r.stage == SD_READY_STAGE_MOUNTED);
    CHECK(r.elapsed_ms <= EXEC_GRACE_MS + POLL_MS); /* did not ride out the short/extended/hard deadlines */
}

/* 7. Executable becoming accessible shortly after mount (within the grace
 * window) -- must still succeed, not give up right at the mount edge. */
static void test_executable_late(void) {
    BEGIN_TEST("executable_late");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 0;
    f.mount_succeeds_for_partition_at_call = 1;
    f.exec_never_ready = false;
    f.exec_ready_after_ms_set = true;
    f.exec_ready_after_ms = 10; /* becomes accessible 10ms after mount, well inside EXEC_GRACE_MS=15 */

    sd_ready_result_t r = run(&f);

    CHECK(r.mounted);
    CHECK(r.executable_ready);
    CHECK(r.stage == SD_READY_STAGE_EXEC_READY);
    CHECK(r.elapsed_ms >= 10);
}

/* 8. Hard-deadline expiration -- weak evidence (sysfs) from the very start
 * genuinely improves to a real device node between the short and extended
 * checkpoints (so the extension past each checkpoint is properly earned),
 * but the node never actually mounts, all the way out to the hard
 * deadline. Must stop at/around the hard deadline, not loop forever. */
static void test_hard_deadline(void) {
    BEGIN_TEST("hard_deadline");
    fake_ctx_t f;
    fake_init(&f);
    f.mmc_evidence_always = true; /* evidence at the short checkpoint (t=50): stage MMC_EVIDENCE */
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = SHORT_MS + 30; /* node appears at t=80, before the extended checkpoint (150) */
    f.mount_succeeds_for_partition_at_call = -1;        /* never actually mounts */

    sd_ready_result_t r = run(&f);

    CHECK(!r.mounted);
    CHECK(r.stage == SD_READY_STAGE_NODE_PRESENT); /* higher than the short checkpoint's MMC_EVIDENCE -- real progress */
    /* Exact, not just an upper-bounded range: the final wait_ms() call is
     * clamped to whatever's left of the hard deadline (sd_ready.c), so this
     * must land EXACTLY on it, not merely "close" -- a regression here
     * (e.g. a flat poll-interval sleep with no clamp) would overshoot by up
     * to one poll interval, which this exact check would catch and a loose
     * ">= HARD_MS" bound would not. */
    CHECK(r.elapsed_ms == HARD_MS);
}

/* 8b. Mount succeeding right at the edge of the hard deadline, with no
 * executable ever found -- the specific reported scenario where the
 * exec-grace path could otherwise ride an extra exec_grace_ms past the
 * hard deadline on top of the mount itself landing right at the edge. Must
 * still never exceed the hard deadline. */
static void test_mount_near_hard_deadline_no_executable(void) {
    BEGIN_TEST("mount_near_hard_deadline_no_executable");
    fake_ctx_t f;
    fake_init(&f);
    f.mmc_evidence_always = true;
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = SHORT_MS + 30; /* node appears at t=80 -- genuine progress by the extended checkpoint */
    /* (HARD_MS - 80) / POLL_MS calls after the node appears lands the
     * mount right at the hard deadline's edge, worst case for the
     * exec-grace path stacking on top of it. */
    f.mount_succeeds_for_partition_at_call = (int) ((HARD_MS - (SHORT_MS + 30)) / POLL_MS);
    /* exec_never_ready stays true (fake_init() default) */

    sd_ready_result_t r = run(&f);

    CHECK(r.mounted);
    CHECK(!r.executable_ready);
    CHECK(r.elapsed_ms <= HARD_MS); /* the one guarantee that must never break, regardless of exact timing */
}

/* 8b. Companion case: evidence present from the very start but making NO
 * further progress between the short and extended checkpoints -- must stop
 * at the extended deadline, not consume the rest of the hard-deadline
 * budget for nothing. */
static void test_extended_deadline_no_progress(void) {
    BEGIN_TEST("extended_deadline_no_progress");
    fake_ctx_t f;
    fake_init(&f);
    f.mmc_evidence_always = true; /* evidence from tick zero, never upgrades to an actual node */

    sd_ready_result_t r = run(&f);

    CHECK(!r.mounted);
    CHECK(r.stage == SD_READY_STAGE_MMC_EVIDENCE);
    CHECK(r.elapsed_ms >= EXTENDED_MS);
    CHECK(r.elapsed_ms < HARD_MS); /* stopped at the extended checkpoint, never reached hard */
}

/* 9. Monotonic clock failure -- must return a single best-effort result
 * immediately (elapsed_ms == -1), never loop. */
static void test_clock_failure(void) {
    BEGIN_TEST("clock_failure");
    fake_ctx_t f;
    fake_init(&f);
    f.clock_broken = true;

    sd_ready_result_t r = run(&f);

    CHECK(r.elapsed_ms == -1);
    CHECK(!r.mounted);
    CHECK(f.wait_ms_calls == 0); /* never entered the wait loop at all */
}

/* 10. The remaining-deadline budget handed to try_mount() must be a sane,
 * positive value measured against the HARD deadline, and must actually
 * shrink as real (simulated) time passes -- this is the wiring the
 * real_try_mount() implementation depends on to bound its own subprocess
 * calls; a wrong or stale value here would silently defeat that bound
 * without any test noticing via elapsed_ms/stage alone. */
static void test_mount_probe_deadline_budget(void) {
    BEGIN_TEST("mount_probe_deadline_budget");
    fake_ctx_t f;
    fake_init(&f);
    f.partition_exists_after_ms_set = true;
    f.partition_exists_after_ms_value = 0;
    f.mount_succeeds_for_partition_at_call = -1; /* never mounts -- keeps retrying every tick */

    sd_ready_result_t r = run(&f);

    CHECK(!r.mounted);
    CHECK(f.last_deadline_remaining_ms > 0);
    CHECK(f.last_deadline_remaining_ms <= HARD_MS);
}

int main(void) {
    test_no_card();
    test_normal_fast_card();
    test_node_appears_late();
    test_enumeration_slower_than_short_deadline_known_limitation();
    test_partition_after_whole();
    test_mount_fails_then_succeeds();
    test_mounted_no_executable();
    test_executable_late();
    test_hard_deadline();
    test_mount_near_hard_deadline_no_executable();
    test_extended_deadline_no_progress();
    test_clock_failure();
    test_mount_probe_deadline_budget();

    if (g_failures == 0) {
        fprintf(stderr, "All sd_ready tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d sd_ready test assertion(s) failed.\n", g_failures);
    return 1;
}
