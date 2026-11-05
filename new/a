
I've updated the workflow and setup guide for offline runs, and fixed the other offline problems they surfaced. None of it is committed yet.

**One thing to do while you still have internet:** commit and push these changes, then `git pull` on both laptops. Otherwise the laptops will run the old scripts. I can commit and push for you if you want.

## What changed

**[experiment_run_workflow.md](experiment_run_workflow.md)**, rewritten:
- **Offline rules up front:** nothing downloads anything during runs, files move by USB, and unsynced clocks don't matter because latency is measured on the client's monotonic clock.
- **New time budget:** about 2 h 10 min per repetition for all 4 backends. Do Rep 1 for all four first, which gives you all 8 graphs. Then add reps in the rotated backend order as time allows.
- **Session start:** kernel tuning and IRQ affinity don't survive a reboot, so the setup phases have to be re-run after each boot.
- **Step order:** the pilot now comes before the baseline snapshot, so you no longer need to restart the server after it. The pilot fails loudly if fewer than 10000 of 10000 connections are established.
- **Server stop is fixed:** it's now `sudo pkill -x echo-<mech>`. The old `kill $SERVER_PID` was aimed at a root-owned `sudo` wrapper and wouldn't stop the server.
- **New quick client check (Step 9):** a one-line `awk` command lists failures, with rules for which ones are expected (overload points) and which are real problems (light load).
- **Archiving without internet:** plot locally, `git commit` locally, keep a `git bundle` on the USB stick, and push later.

**[linux_network_i_o_benchmark_setup_guide.md](linux_network_i_o_benchmark_setup_guide.md)**:
- **New Phase 0, "While still online":** clone, install, build, and an offline-readiness checklist (compiler, liburing, plot libraries, binaries, self-test, no CRLF). It also covers a laptop that never gets internet: phone tethering for apt, plus a git bundle over USB for the code.
- **Background services:** stops the update and time-sync services that would otherwise keep retrying the network.
- **Fixes to stale content:**
  - The repo path is unified to `~/CS331-T008-NetworkIO`.
  - The loadgen path is `./echo_loadgen`; `./build/echo_loadgen` never existed.
  - The select ceiling is stated as 900 connections.
  - The validity checks now run once per backend instead of after every 30-second run.
- **Removed `server_snapshot.py`:** it blocked for about 25 s and its t10/t40 snapshots don't line up with a 30-minute suite. The before/after counters in the workflow replace it.

**Other offline fixes:**
- **[scripts/install_prerequisites.sh](scripts/install_prerequisites.sh)** now installs `python3-pandas`, `python3-matplotlib` and `python3-numpy` from apt, and checks they import. Before, the plotter would have failed offline, since `pip` needs internet.
- **[plot_results.py](plot_results.py)** now searches for results recursively, so it works directly on the USB copy.
- **Duplicate setup guide removed:** `linux_network_i_o_benchmark_setup_guide (1).md` is staged for deletion. Its only difference was the `server_snapshot.py` note.
- **[LOADGEN_README.md](LOADGEN_README.md)** documented a `-W` flag that the new loadgen rejects; it now lists `--pipeline-depth`.

If one laptop doesn't end up with the plotting libraries, copy `results/` to any machine that has pandas and matplotlib and run `plot_results.py` there; this Windows machine already has both.