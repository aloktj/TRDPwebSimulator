# MD Web Feature Plan

This plan explains how the simulator UI and backend should expose all TRDP MD message styles
so users can exercise the full MD lifecycle (Mn, Mr, Mp, Mq, Mc, Me) directly from the
website.

## Goals
- Let a user compose, send, and monitor every MD type the stack supports: Notification (Mn),
  Request with reply (Mr), Reply without confirmation (Mp), Reply with confirmation (Mq),
  Confirm (Mc), and Error (Me).
- Keep the UI consistent with the existing PD/MD tables while adding MD-specific controls
  such as expected replies/confirmations and timeouts.
- Surface session state (sent, replied, confirmed, timed out, errored) in real time via
  WebSocket updates.

## Backend responsibilities
- **Expose MD metadata**: REST endpoint lists MD telegrams with type, direction, and dataset so
  the UI can render MD-capable rows and forms.
- **Send any MD flavor**: Extend the TelegramController to accept an `mdMode` field indicating
  Mn/Mr/Mp/Mq/Mc/Me. Map it to TRDP send parameters (`tlm_request` for Mr/Mq, `tlm_reply` for
  Mp/Mc/Me, `tlm_notify` for Mn) and supply QoS, timeout, and expected reply counts from the
  request body.
- **Session tracking**: Reuse/extend the existing `trackMdRequest` + `registerMdReply`
  bookkeeping to emit WebSocket notifications for state transitions (sent, reply received,
  confirmation received, timeout, error). Attach a correlation ID so the UI can match replies
  to the originating send action.
- **Dataset marshalling**: Serialize/deserialize payloads from the registry dataset definitions
  exactly as done for PD; the only delta is the MD envelope (ComId, session handle, reply
  expectation).
- **Error surfacing**: Map TRDP error callbacks to an Me WebSocket event with human-readable
  text so the UI can display MD errors inline.

## Frontend user flow
- **Telegram list**: Mark MD telegrams with a badge and a “Send MD” action. Disable send for
  RX-only MD definitions.
- **MD composer dialog**:
  - Fields table: same dataset table used for PD editing (type-aware inputs, array editors).
  - Mode selector: Mn/Mr/Mp/Mq/Mc/Me radio group with contextual help text (e.g., “Mr expects
    reply + confirmation”).
  - Options panel: destination IP/port override, expected replies count, confirmation timeout,
    overall request timeout, TCP/UDP toggle if supported by the config.
  - Send button issues REST POST; show progress spinner until first WebSocket update arrives.
- **Live status panel**:
  - Show per-send timeline: sent → reply received (with payload preview) → confirmation → done
    or timeout/error.
  - Allow downloading the raw MD payload for debugging.
  - Provide a “Resend with same payload” shortcut to iterate quickly.

## Coverage of all MD patterns
- **Mn (Notification)**: one-way send; status ends after transmission or stack error.
- **Mr (Request with reply)**: expect one or more replies; close when all replies and optional
  confirmations arrive or time out.
- **Mp (Reply without confirmation)**: reply payload sender; status tracked only for send/error.
- **Mq (Reply with confirmation)**: send reply and wait for confirmation callback before
  completion.
- **Mc (Confirm)**: send confirmation for a received Mr/Mq; UI binds to an existing session ID.
- **Me (Error)**: send explicit error responses and surface inbound error callbacks as Me status
  in the timeline.

## Observability & test hooks
- Add MD-specific log category to trace send/reply/confirm/error events.
- Provide a “Simulate MD reply/confirm/error” developer control in non-production builds to
  validate UI flows without hardware.
- Record metrics: counts of each MD mode sent/received, average latency to first reply, and
  timeout frequency, exposed via a `/metrics` endpoint.
