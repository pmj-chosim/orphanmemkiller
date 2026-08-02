import os
import sys
import time
import numpy as np
from multiprocessing import shared_memory

# Classic double-fork daemonize: intentionally detach from the launching
# shell so the daemon legitimately outlives its parent and keeps a /dev/shm
# segment alive. This is the "what if the orphan is doing important work
# on purpose" adversarial case -- structurally identical to a leaked
# worker (ppid becomes 1, holds a /dev/shm fd), but by design, not by bug.

mb_size = int(sys.argv[1]) if len(sys.argv) > 1 else 64
size_bytes = mb_size * 1024 * 1024

pid = os.fork()
if pid > 0:
    sys.exit(0)  # first parent exits immediately

os.setsid()

pid = os.fork()
if pid > 0:
    sys.exit(0)  # second parent exits; daemon is now a true orphan by design

shm = shared_memory.SharedMemory(create=True, size=size_bytes)
arr = np.ndarray((size_bytes,), dtype=np.uint8, buffer=shm.buf)
arr.fill(2)
try:
    os.unlink("/dev/shm/" + shm.name)
except Exception:
    pass

with open("/workspace/legit_daemon.pid", "w") as f:
    f.write(str(os.getpid()))

while True:
    time.sleep(1)
