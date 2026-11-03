# CS331-T008 Benchmarking Runbook

This runbook gives the exact commands in sequential order to run the automated echo-server benchmarking suite using `scripts/run_experiments.sh`.

---

## 1. One-Time Setup & Build

### On Both Machines (Server & Client):
Ensure latest code and rebuild binaries:
```bash
cd ~/linux-network-io-bench
git pull origin loadgen-bench
make clean && make echo && make loadgen
```

### On Client Machine:
Set up passwordless SSH to the Server (Omen at `192.168.50.1`):
```bash
./scripts/setup_ssh.sh koko 192.168.50.1
```
*(Verify by running: `ssh koko@192.168.50.1 "echo OK"` — should print `OK` without asking for a password).*

---

## 2. Dry Run Verification (Client)

Verify the test grid, SSH, server binaries, and parameters without sending traffic:
```bash
./scripts/run_experiments.sh --dry-run
```
Output will print the 49 scheduled configurations and confirm all pre-flight checks passed.

---

## 3. Phase 2: Pilot Run (Quick Sanity Calibration)

Run only a single round of Experiment E1 to verify end-to-end telemetry and timing:
```bash
./scripts/run_experiments.sh --only E1 --rounds 1
```
- Verifies that servers launch over SSH, port bind succeeds, loadgen records CSV rows, server process snapshots (CPU, memory, counters) are captured, and server teardown works cleanly.

---

## 4. Phase 3: Full Experiment Sweep (Hard Cap ≤ 3.0 Hours)

Run the complete 49-configuration matrix with 5 repeats across randomized rounds:
```bash
./scripts/run_experiments.sh --max-hours 3.0 --rounds 5
```

### Monitoring Progress:
The runner automatically logs progress to the terminal and `results/run.log`.
After each round, it prints:
- Completed runs and invalid run count.
- Elapsed time and projected total finish time.
- If invalid runs occur (e.g. dropped packets), it automatically retries up to 2 times.

---

## 5. Resuming an Interrupted Sweep

If the run is interrupted (Ctrl+C, power, timeout), simply re-run the same command:
```bash
./scripts/run_experiments.sh --max-hours 3.0 --rounds 5
```
The script automatically reads `results/master.csv` and **skips any configurations and repeats that are already completed and valid**.

---

## 6. Copying Results Out by USB Drive

Once the sweep finishes:
1. Plug your FAT32/exFAT USB flash drive into the Client laptop.
2. Copy the `results/` folder to the USB drive:
```bash
USB_MOUNT=$(ls -d /media/$USER/* | head -n 1)
cp -r ~/linux-network-io-bench/results "$USB_MOUNT/"
sync
```
3. Unplug USB drive and plug it into Laptop 3 for immediate report generation.
