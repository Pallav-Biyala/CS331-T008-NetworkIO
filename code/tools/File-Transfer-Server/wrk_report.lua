-- wrk -s code/tools/File-Transfer-Server/wrk_report.lua: after wrk's normal report, print
-- machine-readable totals and latency percentiles (microseconds) that
-- run_experiment parses. wrk's --latency table stops at p50/p75/p90/p99.
done = function(summary, latency, requests)
   local errors = summary.errors
   io.write(string.format("wrk_requests %d\n", summary.requests))
   io.write(string.format("wrk_bytes %d\n", summary.bytes))
   io.write(string.format("wrk_duration_us %d\n", summary.duration))
   io.write(string.format("wrk_errors_total %d\n",
      errors.connect + errors.read + errors.write + errors.status + errors.timeout))
   for _, p in ipairs({50, 90, 95, 99}) do
      io.write(string.format("wrk_latency_p%d_us %d\n", p, latency:percentile(p)))
   end
   io.write(string.format("wrk_latency_p99_9_us %d\n", latency:percentile(99.9)))
   io.write(string.format("wrk_latency_max_us %d\n", latency.max))
end
