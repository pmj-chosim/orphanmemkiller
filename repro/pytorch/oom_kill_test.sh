#!/bin/bash
pkill -9 -f dataloader_running.py 2>/dev/null
pkill -9 -f dataloader_crash 2>/dev/null
sleep 1
rm -f /workspace/parent.pid /workspace/run.log
nohup python -u /workspace/dataloader_running.py > /workspace/run.log 2>&1 &
MAIN=$!

# wait for workers to actually appear (real spawn, not just main process start)
for i in $(seq 1 100); do
  WORKERS=$(ps --ppid $MAIN -o pid= 2>/dev/null)
  if [ -n "$WORKERS" ]; then break; fi
  sleep 0.05
done

echo "MAIN=$MAIN WORKERS=$WORKERS"
echo "--- before kill ---"
ps -eo pid,ppid,stat,args | grep -v grep
df -h /dev/shm

echo "--- external kill -9 on MAIN (simulating OOM-killer) ---"
kill -9 $MAIN

for t in 0.1 0.3 0.5 1 2 3 5; do
  sleep $t
  echo "=== t+cumulative ~${t}s more ==="
  ps -eo pid,ppid,stat,args | grep -v grep
  df -h /dev/shm | tail -1
done
