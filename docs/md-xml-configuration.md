# MD configuration in TRDP XML

The simulator’s TRDP XML defines MD telegrams in two parts: datasets that describe the binary payload layout and telegram entries that bind those datasets to MD traffic. Using the default configuration as reference:

- **Define MD payload schemas in `<datasets>`**. Each MD payload gets a named dataset with typed variables, sizes, and offsets that match the on-wire structure (e.g., `MdRequest` and `MdResponse` datasets describe IDs, lengths, payload bytes, and CRC markers).【F:configs/default.xml†L18-L37】
- **Declare MD telegrams in `<telegrams>`**. MD telegram entries set `type="MD"`, a `direction` of `TX` or `RX`, the dataset to marshal, and TRDP addressing fields (`comId`, `ttl`, `port`, `srcIp`, `destIp`). For example, `MdMaintenanceRequest` is a TX MD telegram using the `MdRequest` dataset, while `MdMaintenanceResponse` is an RX MD telegram using `MdResponse`.【F:configs/default.xml†L43-L52】

### Mapping XML to MD behavior
- **MD modes (Mn/Mr/Mp/Mq/Mc/Me)** are driven by how the engine uses these telegram definitions: TX MD entries provide the templates for requests or replies, and RX MD entries describe what to parse when callbacks arrive. Confirmation and error handling are applied by the runtime using the `comId`, dataset, and direction you specify.
- **Per-telegram transport knobs** come from attributes on the XML node. Adjust `ttl`, `port`, and IPs to steer QoS and addressing for each MD flow; add more MD telegram entries to cover additional ComIds or role pairs.
- **Dataset reuse** lets you share schemas across multiple MD telegrams. For instance, the same `MdRequest` dataset could back Mn/Mr traffic toward different endpoints by defining multiple `type="MD"` telegram nodes with distinct `comId` or addressing.

### Steps to extend MD coverage in XML
1. Add or extend datasets for any new MD payload structures (requests, replies, confirmations, or error wrappers).
2. Create matching `<telegram>` entries with `type="MD"` for each role you need (Tx request, Rx reply, Tx confirmation, etc.), wiring them to the datasets from step 1 and setting TRDP addressing/TTL as required.
3. Reload the configuration in the simulator; the engine will open MD sessions for the declared telegrams and bind listeners so all MD patterns are serviced according to your XML.
