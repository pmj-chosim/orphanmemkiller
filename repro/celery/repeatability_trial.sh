#!/bin/bash
N=${1:-15}
PASS=0
FAIL=0
FP=0

# baseline: remember celery worker pids so we can detect if any of them
# ever changes (would mean the detector killed and something respawned it,
# or it died outright)
CELERY_PIDS_BEFORE=$(ps --ppid 12404 -o pid= 2>/dev/null | tr -d ' ' | sort)

for i in $(seq 1 $N); do
  pkill -9 -f leak_maker.py 2>/dev/null
  sleep 0.5
  rm -f /workspace/leak.log
  nohup python -u /workspace/leak_maker.py 100 > /workspace/leak.log 2>&1 &
  MAIN=$!

  # wait for the worker child to actually appear
  WORKER=""
  for j in $(seq 1 50); do
    WORKER=$(ps --ppid $MAIN -o pid= 2>/dev/null | tail -1 | tr -d ' ')
    if [ -n "$WORKER" ]; then break; fi
    sleep 0.05
  done

  if [ -z "$WORKER" ]; then
    echo "TRIAL $i: SETUP_FAILED (no worker spawned)"
    FAIL=$((FAIL+1))
    continue
  fi

  # let the worker actually be a worker for a few seconds (matches every
  # real scenario we tested -- and stays clear of MIN_CHILD_AGE_SEC=2,
  # which deliberately treats a child killed within ~2s of its own birth
  # as launch-handoff noise, not a real orphan)
  sleep 3

  kill -9 $MAIN

  # wait through grace(2s) + kill-grace(3s) + margin
  sleep 7

  # ground truth is /dev/shm usage, not "does the pid still exist" --
  # kill -0 on a zombie (a worker that was correctly SIGTERM'd/SIGKILLed
  # but never reaped by PID 1's `sleep infinity`) returns success even
  # though it no longer holds any memory at all. Baseline with only
  # Redis+Celery running is ~16K; anything meaningfully above that means
  # the 100MB block is still leaked.
  SHM_USED_KB=$(df /dev/shm | tail -1 | awk '{print $3}')
  REDIS_OK=$(redis-cli ping 2>/dev/null)
  CELERY_PIDS_NOW=$(ps --ppid 12404 -o pid= 2>/dev/null | tr -d ' ' | sort)

  RESULT="OK"
  if [ "$SHM_USED_KB" -gt 2048 ]; then RESULT="LEAK_NOT_RECLAIMED"; FAIL=$((FAIL+1)); else PASS=$((PASS+1)); fi
  if [ "$REDIS_OK" != "PONG" ]; then RESULT="$RESULT+REDIS_DOWN"; FP=$((FP+1)); fi
  if [ "$CELERY_PIDS_NOW" != "$CELERY_PIDS_BEFORE" ]; then RESULT="$RESULT+CELERY_PIDS_CHANGED"; FP=$((FP+1)); fi

  echo "TRIAL $i: worker=$WORKER shm_used_kb=$SHM_USED_KB redis=$REDIS_OK result=$RESULT"
done

echo "=== SUMMARY: $PASS/$N leaks reclaimed, $FAIL failures, $FP false-positive incidents (redis/celery touched) ==="
