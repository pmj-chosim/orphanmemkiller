import sys
import os
import time
import numpy as np
from multiprocessing import shared_memory

mb_size = int(sys.argv[1])
size_bytes = mb_size * 1024 * 1024

shm = shared_memory.SharedMemory(
    create=True,
    size=size_bytes
)
arr = np.ndarray(
    (size_bytes,),
    dtype=np.uint8,
    buffer=shm.buf
)
arr.fill(1)

try:
    os.unlink("/dev/shm/" + shm.name)
except Exception:
    pass

pid = os.fork()

if pid == 0:
    print(f"[Worker PID {os.getpid()}] {mb_size}MB allocated.")
    while True:
        time.sleep(1)
else:
    with open("/workspace/parent.pid", "w") as f:
        f.write(str(os.getpid()))
    print(f"[Parent PID {os.getpid()}]")
    while True:
        time.sleep(1)
