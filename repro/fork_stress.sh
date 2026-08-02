#!/bin/bash
N=${1:-1000}
time (for i in $(seq 1 $N); do /bin/true; done)
