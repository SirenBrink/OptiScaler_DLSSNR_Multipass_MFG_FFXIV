# Restore targeted Ada temporal correction — 2026-09-20

RTX 40 testers reported more frequent gameplay crashes with the broad Blackwell kernel rewrite. Restore the targeted temporal correction from the lifecycle-hardened implementation at 48ddacb4. This is a rollback for comparison, not proof that all reported crashes have the same cause.

The Ada-only architecture gate, provider identity checks, module retention, deferred scanning, capability-gate validation and rollback protections remain. Other community integration changes are unchanged. The menu again describes targeted temporal correction.

The current sdli1995 companion runtime (0.3.5) documents RTX 20/30 support, not a supported separate SM89 replacement. Keep external MFG unlockers out of this comparison.

Test with the same driver, runtime DLLs, INI and fixed 3X factor used for the broad-rewrite run. Restart before testing; live replacement cannot undo patches in the already-running process. Save the log after the session. Local patch tests and compilation cannot validate long-session RTX 40 stability.
