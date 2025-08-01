#!/bin/sh

# Init entrypoint for bare-metal devices; calls common init code.

# First stage: very basic setup to bring up network and /dev etc
/init-stage1.sh
INIT_STAGE1_EXIT_CODE=$?

export CURRENT_SECTION=dut_boot

# Second stage: run jobs
test $INIT_STAGE1_EXIT_CODE -eq 0 && /init-stage2.sh

# Wait until the job would have timed out anyway, so we don't spew a "init
# exited" panic.
sleep 6000
