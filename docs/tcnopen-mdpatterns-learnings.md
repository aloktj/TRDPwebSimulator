# Learnings from TCNopen `trdp/test/mdpatterns`

This note summarizes patterns and behaviors observed in the TRDP MD tests shipped with the TCNopen stack (path: `trdp/test/mdpatterns`).

## Test matrix
- `patterns.txt` defines MD coverage for notification, request/reply, and request/reply/confirm across UDP (unicast and multicast) and TCP, with both 64-byte and 32-kB payloads.
- The same matrix is run on Linux and Windows, validating TCNopen against the UniControls TRDP implementation in both directions.

## Caller behaviors (repetition tests)
- The caller throttles each COMID with mutex-protected flags so a new `tlm_request` is sent only after the prior exchange completes or times out; requests cover standard replies, stats replies, missing-listener errors, topology mismatch, and an infinite-timeout case.
- The MD callback enables confirmations selectively, toggling every few replies, and re-arms call flags on success, reply timeouts, or missing-listener (`ME`) indications to keep the loop running.

## Replier behaviors (repetition tests)
- The replier answers MQ requests with `tlm_replyQuery` and MP/statistics requests with `tlm_reply`, embedding live stack statistics into reply payloads before sending.
- Topology-mismatch and infinite-timeout scenarios are exercised: the replier still issues replies for topology counter changes and can emit MQ replies that wait for confirmation timeouts to be reported by the stack.

## Comprehensive MD pattern harness
- `trdp-md-test.c` enumerates all MD patterns (notify, request/reply, request/reply/confirm) for TCP, UDP unicast, and UDP multicast, and allows grouping tests by protocol/pattern combinations.
- Test options cover message size, timeout, URIs, source/destination IPs, multicast group, and the expected number of multicast repliers, while status tracking records per-pattern errors and reply counts.
- A queue of request records (send, next-test, status, exit) coordinates caller/replier flows, enabling serialized processing of MD events during the test cycles.
