# TRDP Stack Integration Task Sequence

This plan enumerates the ordered tasks for wiring the TRDP stack into the simulator application, covering session lifecycle, PD/MD data paths, TAU helpers, and ECSP control. Follow the steps sequentially to minimize churn and testing overhead.

## 1. Environment and Dependency Preparation
- Add TRDP and TAU SDK dependencies to the build system (CMake find modules or imported targets).
- Wire include/library paths for `trdp_if_light.h`, `tau_ctrl.h`, and `tau_dnr.h` and validate versions against application requirements.
- Provide configurable runtime parameters (hosts file path, ECSP enablement, redundancy defaults) in the simulator configuration schema.

## 2. Core Session Lifecycle
- Initialize the library once via `tlc_init()` during simulator startup, supplying marshalling, PD/MD defaults, and process parameters.
- Open an application session with `tlc_openSession()` and store the handle in the runtime context for PD and MD operations.
- Implement reconfiguration hooks using `tlc_configSession()` / `tlc_updateSession()` for dynamic parameter changes.
- On shutdown, close the session with `tlc_closeSession()` and terminate the library via `tlc_terminate()`.

## 3. Event Loop Integration
- Extend the main loop to call `tlc_getInterval()` (or PD-only `tlp_getInterval()`) to derive timeouts and FDs for `select`/`poll`.
- Invoke `tlc_process()` on each loop iteration (or `tlp_processReceive()` / `tlp_processSend()` where applicable) to service timers and sockets.
- Track topology counters via `tlc_setETBTopoCount()` and `tlc_setOpTrainTopoCount()` when topology changes are detected.

## 4. Periodic Data (PD) Path
- Implement publisher setup using `tlp_publish()`; support SOA republish with `tlp_republish()` / `tlp_republishService()`.
- Implement subscriber setup via `tlp_subscribe()` / `tlp_resubscribe()` with callbacks to minimize copies and per-stream send/receive parameters for priorities and redundancy.
- Add low-jitter send support with `tlp_putImmediate()` for timestamped frames; fall back to `tlp_put()` for regular sends.
- Support redundant paths using `tlp_setRedundant()` / `tlp_getRedundant()` and `redId` fields in publish/subscribe operations.
- Provide request/response helpers combining `tlp_request()` and `tlp_get()` for synchronous-style PD exchanges.

## 5. Message Data (MD) Path
- Register unsolicited notification handlers using `tlm_notify()` and request/reply handlers via `tlm_request()` with bounded `numReplies` and `replyTimeout`.
- Add shared listener management using `tlm_addListener()` and `tlm_readdListener()` to reuse sockets across MD consumers.
- Implement replies through `tlm_reply()` / `tlm_replyQuery()` using session IDs and optional confirm timeouts to avoid extra buffering.

## 6. TAU DNR and Topology Utilities
- Initialize DNR once with `tau_initDnr()` (choosing common-thread or dedicated-thread mode); expose hosts file configuration.
- Add URI/IP conversion helpers through `tau_uri2Addr()` / `tau_ipFromURI()` and `tau_addr2Uri()` with caching enabled.
- Implement topology-aware ID conversions (e.g., `tau_label2Ids()`, `tau_label2TcnVehNo()`, `tau_addr2Ids()`, `tau_addr2TcnVehNo()`) for dynamic consist handling.
- Provide consist/vehicle mapping helpers (`tau_tcnCstNo2CstId()`, `tau_opCstNo2CstId()`, `tau_label2CstId()`) to feed PD/MD configuration.

## 7. ECSP Control for Topology Changes
- Initialize ECSP control channel using `tau_initEcspCtrl()` and expose parameters in configuration.
- Implement control updates with `tau_setEcspCtrl()` and status polling via `tau_getEcspStat()`.
- Add confirmation request/reply handling using `tau_requestEcspConfirm()` and `tau_requestEcspConfirmReply()` with callbacks integrated into the main loop.

## 8. Configuration and Runtime Glue
- Map simulator configuration fields to TRDP defaults (PD/MD parameters, redundancy settings, timeouts, ECSP enablement).
- Add logging and metrics around session lifecycle, PD/MD operations, and TAU lookups for observability.
- Ensure buffers and marshalling align with application data models to avoid extra copies; reuse shared listeners/publishers.

## 9. Testing and Validation
- Create unit and integration tests covering session setup/teardown, PD publish/subscribe, MD request/response, DNR lookups, and ECSP control flows.
- Build end-to-end simulator scenarios exercising redundancy, topology counter updates, and URI/IP resolution paths.
- Document operational runbook for configuring TRDP features and monitoring health in deployment.
