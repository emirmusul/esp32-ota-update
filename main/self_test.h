#pragma once

#include <stdbool.h>

/**
 * Run the post-boot health check and settle the rollback decision.
 *
 * When the running image is in PENDING_VERIFY state, this either marks it
 * valid (cancelling rollback) or invalidates it and reboots into the
 * previous slot. When rollback is not pending, the check still runs and
 * its result is logged, but nothing is marked.
 *
 * Blocks for up to CONFIG_SELF_TEST_TIMEOUT_MS waiting for the sensor.
 *
 * @return true if the health check passed. Does not return when the
 *         image is rolled back.
 */
bool self_test_run(void);