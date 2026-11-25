# Rashomon

A memory-order judge for C++ concurrent code: an AI writes the implementation against a spec, Rashomon answers PASS / FAIL (with a counterexample) / INCONCLUSIVE.

*The name:* in Kurosawa's *Rashomon* every witness tells the same events in a different order, and none of them can be taken at their word. Threads under a weak memory model do the first; the judges that check them can do the second. So there are two independent judges, calibrated on cases whose answer is known. (Formerly `mocheck`.)

- Tool, specs and full documentation: [`rashomon/README.md`](rashomon/README.md)
- Real-world library audit: [`realworld/`](realworld/)
- Status and next steps: [`HANDOFF.md`](HANDOFF.md)
