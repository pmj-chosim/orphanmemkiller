import os
import sys
import time
import torch
from torch.utils.data import DataLoader, Dataset


class DummyDataset(Dataset):
    def __init__(self, size=10000, tensor_size=65536):
        self.size = size
        self.tensor_size = tensor_size

    def __len__(self):
        return self.size

    def __getitem__(self, idx):
        return torch.ones(self.tensor_size)


def main():
    with open("/workspace/parent.pid", "w") as f:
        f.write(str(os.getpid()))

    ds = DummyDataset()
    loader = DataLoader(ds, batch_size=64, num_workers=4, persistent_workers=True)
    it = iter(loader)

    for i in range(3):
        batch = next(it)
        print(f"[Main PID {os.getpid()}] got batch {i}, shape={tuple(batch.shape)}", flush=True)
        time.sleep(1)

    try:
        print(f"[Main PID {os.getpid()}] about to hard-crash (os._exit, no cleanup, "
              f"simulating a fatal signal / native crash that skips __del__/atexit)", flush=True)
        raise RuntimeError("Simulated unrecoverable training crash")
    except RuntimeError:
        os._exit(1)  # bypass interpreter shutdown / DataLoader.__del__ entirely


if __name__ == "__main__":
    main()
