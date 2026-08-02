#!/bin/bash
set -x
pkill -9 -f dataloader_crash_hard.py 2>/dev/null
sleep 1
rm -f /workspace/run_hard.log
nohup python -u /workspace/dataloader_crash_hard.py > /workspace/run_hard.log 2>&1 &
MAIN=$!

# poll rapidly until at least one worker (child of MAIN) shows up
for i in $(seq 1 100); do
  WORKERS=$(ps --ppid $MAIN -o pid= 2>/dev/null)
  if [ -n "$WORKERS" ]; then
    break
  fi
  sleep 0.05
done

echo "MAIN=$MAIN WORKERS=$WORKERS"

# freeze every worker so it can never run its own ppid self-check
for w in $WORKERS; do
  kill -STOP "$w"
done

# now kill main
kill -9 "$MAIN"

sleep 1
echo "--- state right after freeze+kill ---"
ps -eo pid,ppid,stat,args | grep -v grep
df -h /dev/shm
