#!/usr/bin/env bash
# shellcheck disable=SC2035 # FIXME glob
# shellcheck disable=SC2086 # we want word splitting
# shellcheck disable=SC1091 # paths only become valid at runtime

. "${SCRIPTS_DIR}/setup-test-env.sh"

section_start traces_prepare "traces: preparing test setup"

set -ex

# Our rootfs may not have "less", which apitrace uses during apitrace dump
export PAGER=cat  # FIXME: export everywhere

# Check we're using the version of Piglit we think we are
ci_tag_test_time_check "PIGLIT_TAG"

INSTALL=$(realpath -s "$PWD"/install)

export PIGLIT_REPLAY_DESCRIPTION_FILE="$INSTALL/$PIGLIT_TRACES_FILE"

# FIXME: guess why /usr/local/bin is not included in all runners PATH.
# Needed because yq and ci-fairy are installed there.
PATH="/usr/local/bin:$PATH"

if [ "$PIGLIT_REPLAY_SUBCOMMAND" = "profile" ]; then
    yq -iY 'del(.traces[][] | select(.label[]? == "no-perf"))' \
      "$PIGLIT_REPLAY_DESCRIPTION_FILE"
else
    # keep the images for the later upload
    export PIGLIT_REPLAY_EXTRA_ARGS="--keep-image ${PIGLIT_REPLAY_EXTRA_ARGS}"
fi

# Set up the environment.
# Modifiying here directly LD_LIBRARY_PATH may cause problems when
# using a command wrapper. Hence, we will just set it when running the
# command.
export __LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$INSTALL/lib/"
if [ -n "${VK_DRIVER}" ]; then
  ARCH=$(uname -m)
  export VK_DRIVER_FILES="$INSTALL/share/vulkan/icd.d/${VK_DRIVER}_icd.$ARCH.json"
fi

# Sanity check to ensure that our environment is sufficient to make our tests
# run against the Mesa built by CI, rather than any installed distro version.
MESA_VERSION=$(head -1 "$INSTALL/VERSION" | sed 's/\./\\./g')

# wrapper to supress +x to avoid spamming the log
quiet() {
    set +x
    "$@"
    set -x
}

# Set environment for apitrace executable.
export PATH="/apitrace/build:$PATH"

echo "Version:"
apitrace version 2>/dev/null || echo "apitrace not found (Linux)"

SANITY_MESA_VERSION_CMD="wflinfo"

HANG_DETECTION_CMD=""

# Set up the platform windowing system.
if [ "$EGL_PLATFORM" = "surfaceless" ]; then
    # Use the surfaceless EGL platform.
    export DISPLAY=
    export WAFFLE_PLATFORM="surfaceless_egl"

    SANITY_MESA_VERSION_CMD="$SANITY_MESA_VERSION_CMD --platform surfaceless_egl --api gles2"

    if [ "$GALLIUM_DRIVER" = "virpipe" ]; then
    # piglit is to use virpipe, and virgl_test_server llvmpipe
    export GALLIUM_DRIVER="$GALLIUM_DRIVER"

    LD_LIBRARY_PATH="$__LD_LIBRARY_PATH" \
    GALLIUM_DRIVER=llvmpipe \
    VTEST_USE_EGL_SURFACELESS=1 \
    VTEST_USE_GLES=1 \
    virgl_test_server >"$RESULTS_DIR"/vtest-log.txt 2>&1 &

    sleep 1
    fi
elif [ "$PIGLIT_PLATFORM" = "gbm" ]; then
    SANITY_MESA_VERSION_CMD="$SANITY_MESA_VERSION_CMD --platform gbm --api gl"
elif [ "$PIGLIT_PLATFORM" = "mixed_glx_egl" ]; then
    # It is assumed that you have already brought up your X server before
    # calling this script.
    SANITY_MESA_VERSION_CMD="$SANITY_MESA_VERSION_CMD --platform glx --api gl"
else
    SANITY_MESA_VERSION_CMD="$SANITY_MESA_VERSION_CMD --platform glx --api gl --profile core"
    # copy-paste from init-stage2.sh, please update accordingly
    {
      WESTON_X11_SOCK="/tmp/.X11-unix/X0"
      export WAYLAND_DISPLAY=wayland-0
      export DISPLAY=:0
      mkdir -p /tmp/.X11-unix

      env weston --config="/install/common/weston.ini" -Swayland-0 --use-gl &

      while [ ! -S "$WESTON_X11_SOCK" ]; do sleep 1; done
    }
fi

# If the job is parallel at the  gitlab job level, will take the corresponding
# fraction of the caselist.
if [ -n "$CI_NODE_INDEX" ]; then
    USE_CASELIST=1
fi

# shellcheck disable=SC2317
replay_s3_upload_images() {
    find "$RESULTS_DIR/$__PREFIX" -type f -name "*.png" -printf "%P\n" \
        | while read -r line; do

        __TRACE="${line%-*-*}"
        if grep -q "^$__PREFIX/$__TRACE: pass$" ".gitlab-ci/piglit/$PIGLIT_RESULTS.txt.orig"; then
            if [ "x$CI_PROJECT_PATH" != "x$FDO_UPSTREAM_REPO" ]; then
                continue
            fi
            __S3_PATH="$PIGLIT_REPLAY_REFERENCE_IMAGES_BASE"
            __DESTINATION_FILE_PATH="${line##*-}"
            if curl --fail -L -s -I "https://${__S3_PATH}/${__DESTINATION_FILE_PATH}" | grep -Eq "^content-type: (binary|application)\/octet-stream" 2>/dev/null; then
                continue
            fi
        else
            __S3_PATH="$JOB_ARTIFACTS_BASE"
            __DESTINATION_FILE_PATH="$__S3_TRACES_PREFIX/${line##*-}"
        fi

        ci-fairy s3cp --token-file "${S3_JWT_FILE}" "$RESULTS_DIR/$__PREFIX/$line" \
            "https://${__S3_PATH}/${__DESTINATION_FILE_PATH}"
    done
}

SANITY_MESA_VERSION_CMD="$SANITY_MESA_VERSION_CMD | tee /tmp/version.txt | grep \"Mesa $MESA_VERSION\(\s\|$\)\""

cd $RESULTS_DIR && rm -rf ..?* .[!.]* *
cd /piglit

if [ -n "$USE_CASELIST" ]; then
    PIGLIT_TESTS=$(printf "%s" "$PIGLIT_TESTS")
    PIGLIT_GENTESTS="./piglit print-cmd $PIGLIT_TESTS replay --format \"{name}\" > /tmp/case-list.txt"
    RUN_GENTESTS="export LD_LIBRARY_PATH=$__LD_LIBRARY_PATH; $PIGLIT_GENTESTS"

    eval $RUN_GENTESTS

    sed -ni $CI_NODE_INDEX~$CI_NODE_TOTAL"p" /tmp/case-list.txt

    PIGLIT_TESTS="--test-list /tmp/case-list.txt"
fi

PIGLIT_OPTIONS=$(printf "%s" "$PIGLIT_OPTIONS")

PIGLIT_TESTS=$(printf "%s" "$PIGLIT_TESTS")

PIGLIT_CMD="./piglit run -l verbose --timeout 300 -j${FDO_CI_CONCURRENT:-4} $PIGLIT_OPTIONS $PIGLIT_TESTS replay "$(/usr/bin/printf "%q" "$RESULTS_DIR")

RUN_CMD="export LD_LIBRARY_PATH=$__LD_LIBRARY_PATH; $SANITY_MESA_VERSION_CMD && $HANG_DETECTION_CMD $PIGLIT_CMD"

# The replayer doesn't do any size or checksum verification for the traces in
# the replayer db, so if we had to restart the system due to intermittent device
# errors (or tried to cache replayer-db between runs, which would be nice to
# have), you could get a corrupted local trace that would spuriously fail the
# run.
rm -rf replayer-db

# ANGLE: download compiled ANGLE runtime and the compiled restricted traces (all-in-one package)
if [ -n "$PIGLIT_REPLAY_ANGLE_ARCH" ]; then
  FILE="angle-bin-${PIGLIT_REPLAY_ANGLE_ARCH}-${ANGLE_TRACE_FILES_TAG}.tar.zst"
  curl --location --fail --retry-all-errors --retry 4 --retry-delay 60 \
    --header "Authorization: Bearer $(cat "${S3_JWT_FILE}")" \
    "https://s3.freedesktop.org/mesa-tracie-private/${FILE}" --output "${FILE}"
  mkdir -p replayer-db/angle
  tar --zstd -xf ${FILE} -C replayer-db/angle/
fi

PIGLIT_RESULTS="${PIGLIT_RESULTS:-replay}"
RESULTSFILE="$RESULTS_DIR/$PIGLIT_RESULTS.txt"
mkdir -p .gitlab-ci/piglit

uncollapsed_section_switch traces "traces: run traces"

if ! eval $RUN_CMD;
then
    error "Found $(cat /tmp/version.txt), expected $MESA_VERSION"
fi


./piglit summary aggregate "$RESULTS_DIR" -o junit.xml

{ set +x; } 2>/dev/null
./piglit summary console "$RESULTS_DIR"/results.json.bz2 \
    | tee ".gitlab-ci/piglit/$PIGLIT_RESULTS.txt.orig" \
    | head -n -1 | grep -v ": pass" \
    | sed '/^summary:/Q' \
    > $RESULTSFILE

if [ -s $RESULTSFILE ]; then
	error "Failures in traces:"
	cat $RESULTSFILE
	echo "Review the image changes and get the new checksums at: ${ARTIFACTS_BASE_URL}/results/summary/problems.html"
	echo "If the new traces look correct to you, you can update the checksums"
	echo "locally by running:"
	echo "    ./bin/ci/update_traces_checksum.sh"
	echo "and resubmit this merge request."
fi

section_switch test_post_process "traces: post-processing test results"

__PREFIX="trace/$PIGLIT_REPLAY_DEVICE_NAME"
__S3_PATH="$PIGLIT_REPLAY_ARTIFACTS_BASE_URL"
__S3_TRACES_PREFIX="traces"

set -x

if [ "$PIGLIT_REPLAY_SUBCOMMAND" != "profile" ]; then
    quiet replay_s3_upload_images
fi


if [ ! -s $RESULTSFILE ]; then
    rm -rf "${RESULTS_DIR:?}/${__PREFIX}"
    { set +x; } 2>/dev/null
    section_end test_post_process
    exit 0
fi

./piglit summary html --exclude-details=pass \
"$RESULTS_DIR"/summary "$RESULTS_DIR"/results.json.bz2

find "$RESULTS_DIR"/summary -type f -name "*.html" -print0 \
        | xargs -0 sed -i 's%<img src="file://'"${RESULTS}"'.*-\([0-9a-f]*\)\.png%<img src="https://'"${JOB_ARTIFACTS_BASE}"'/traces/\1.png%g'
find "$RESULTS_DIR"/summary -type f -name "*.html" -print0 \
        | xargs -0 sed -i 's%<img src="file://%<img src="https://'"${PIGLIT_REPLAY_REFERENCE_IMAGES_BASE}"'/%g'

section_end test_post_process

exit 1
