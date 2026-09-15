# Link State Fabric

Link State Fabric is the authoritative dynamic link-condition runtime of the
Distributed Fabric Infrastructure stack. It answers one question:

> What is the current authoritative operational state of this exact fabric link
> right now, which evidence supports that state, how current is that evidence,
> which process is authorized to publish or change it, and when must that state
> be rejected, invalidated, superseded, fenced, or reduced to UNKNOWN?

It is a C++20 library, a coordinator/worker runtime, and a CMake package. It is
vendor neutral: the core state machine makes no assumption about Ethernet,
InfiniBand, optical or overlay links.

## Systems boundary

Link State Fabric owns:

* authoritative link operational state and its state generation;
* link evidence, evidence generation and currentness;
* publisher authority, worker incarnation fencing and coordinator epochs;
* state transitions, conflict resolution, provenance and lifecycle;
* deterministic state explanations, immutable snapshots and state digests;
* stale-state rejection, explicit UNKNOWN, explicit REVALIDATION_REQUIRED;
* distributed link-state publication, conservative restart and recovery;
* versioned, integrity-checked persistence of durable link state.

Link State Fabric does **not** own, implement or decide:

* canonical identity (Fabric Registry owns identity);
* structural connectivity and the topology graph (Fabric Topology owns structure);
* port identity, port configuration or port lifecycle beyond binding evidence
  (Port Fabric owns those);
* routes, path planning, path legality or bandwidth reservation
  (Route Fabric and Path Authority own those);
* congestion state or congestion control (Network Congestion Fabric owns those);
* QoS, flow scheduling or traffic engineering;
* failure-domain semantics, switch configuration rollout, capability truth
  (Fabric Capability Registry owns capability truth).

A current UP link is not automatically a legal path. A DEGRADED link may still be
usable by later policy layers. Link State Fabric says what the link's operational
condition is under current evidence and authority; it does not decide what
traffic should do about it.

## State model

Exactly one state is authoritative for a link at a time.

| State | Meaning |
| --- | --- |
| UNKNOWN | No decisive current evidence and no durable prior conclusion. Never a synonym for DOWN or UP. |
| UP | Current evidence supports operational availability. |
| DOWN | Current evidence supports a non-operational link. |
| DEGRADED | Operational but materially impaired; the impairment is named by a degradation cause. |
| ADMIN_DISABLED | Intentionally disabled by administrative intent, not by failure. |
| DRAINING | Administratively quiescing: still forwards, must not take new work. |
| FAULTED | A hardware or device fault was reported. |
| REVALIDATION_REQUIRED | A durable prior state exists but no current evidence is authoritative for it. |
| RETIRED | Terminal: the underlying topology edge or link identity was retired. |

Consequences that the runtime enforces and the test suite proves:

* topology presence never implies UP: a freshly bound link is UNKNOWN;
* missing evidence never becomes DOWN; it becomes REVALIDATION_REQUIRED when a
  durable conclusion existed, and stays UNKNOWN when none ever did;
* UNKNOWN is only reachable through evidence or withdrawal resolution, never
  through publisher loss, source loss or topology invalidation;
* RETIRED is a sink: a retired record never returns to a live state.

The complete transition matrix is explicit, total over
(from, to, trigger), and pinned by a test that enumerates all combinations
against an independent re-implementation of the documented contract.

## Evidence model

Evidence is not a boolean. Each record carries:

* the evidence key: link, source, endpoint, direction, kind;
* observation identity, publisher identity, worker boot identity, coordinator
  epoch;
* source generation and observation sequence;
* topology generation and endpoint generation it is bound to;
* `REAL` / `SYNTHETIC` / `UNSUPPORTED` classification;
* the state claim and, for DEGRADED, a named degradation cause;
* a bounded detail string and bounded opaque metadata.

Evidence kinds and their precedence tiers are a stated policy:

| Tier | Kinds |
| --- | --- |
| 1 | AdministrativeState |
| 2 | HardwareErrorIndication |
| 3 | CarrierState, InterfaceOperationalState, DeviceReportedState, PortReportedState, ControlPlaneHeartbeat, LossErrorThreshold, PlatformDiscovery |
| 4 | RemoteEndpointState |
| 5 | SyntheticTest (and any synthetic-class record) |

## Conflict-resolution model

Resolution is rules based, deterministic and explainable. Contradictory evidence
is never averaged and never hidden.

1. Durable administrative intent, then administrative evidence, gates every
   operational claim. Restrictiveness wins over a permissive administrative
   claim from another source and the disagreement is reported.
2. The strongest precedence tier that holds decisive evidence wins; outranked
   evidence is recorded with the tier that outranked it.
3. Within the winning tier: unanimity maps directly; `{UP, DEGRADED}` resolves
   to DEGRADED (impairment evidence outranks an unimpaired claim);
   `{DOWN, FAULTED}` resolves to FAULTED (the specific fault is reported);
   anything else is CONFLICTED.
4. Transmit/receive disagreement from one source and endpoint resolves to
   DEGRADED with cause ASYMMETRY on link classes that declare directional
   evidence, and to CONFLICTED on classes that guarantee symmetry.
5. A remote endpoint that contradicts the local observation resolves to DEGRADED
   with cause ASYMMETRY on directional classes and to CONFLICTED otherwise.

CONFLICTED maps to REVALIDATION_REQUIRED: the runtime refuses to force a binary
answer that the evidence does not support.

## Generation model

Every authoritative commit binds the generations that make it valid:

* link state generation: advances only when the authoritative state changes;
* link evidence generation: advances whenever the current evidence view changes;
* global generation: advances on every durable commit;
* global state generation: advances on every state commit anywhere;
* topology generation and endpoint generations: pinned by every record and by
  every evidence record;
* source generation and observation sequence: per source currentness;
* publisher generation and coordinator epoch: authority currentness.

A stale generation is rejected before mutation. An exact replay of an already
committed observation returns IDEMPOTENT and advances nothing. An older
observation of the same key returns STALE_EVIDENCE.

## Publisher authority, worker fencing and coordinator epochs

* A publisher incarnation is a (`PublisherId`, `WorkerBootId`) pair registered
  for an explicit link scope. There is no implicit wildcard: an empty scope
  grants nothing.
* Registering a new boot for a publisher fences the previous incarnation and
  invalidates the evidence it owned.
* Fencing is permanent. A fenced worker boot can never become active again, even
  if a process presents it after a restart, and the fence registry is rebuilt
  from durable fence records after a coordinator restart.
* Advancing the coordinator epoch supersedes every incarnation of the previous
  epoch and makes its dynamic evidence non-current.
* Losing one publisher recomputes state from the evidence that remains;
  corroborating evidence from another publisher is preserved. When the last
  current evidence disappears the link becomes REVALIDATION_REQUIRED.

## Topology binding and invalidation

Every link-state record binds a `LinkId`, a topology generation and its endpoint
identities with their endpoint generations. When Fabric Topology advances an
edge:

* evidence bound to the previous generation is invalidated, not inherited;
* the record moves to REVALIDATION_REQUIRED (or stays UNKNOWN if no conclusion
  ever existed);
* publication that names the old generation is rejected with STALE_TOPOLOGY.

Logical and tunnel-backed links pin each backing physical link to a topology
generation. A stale backing reference blocks current evidence for the dependent
link, and the dependent link is invalidated when its backing link is superseded
or retired. Logical state is never silently derived from a physical edge.

## Persistence and recovery

The journal is versioned (format version 1), integrity checked (a payload digest
and a whole-file digest), bounds checked, and written atomically through a
temporary file. The decoder rejects bad magic, unsupported versions, truncation
at every length, digest mismatches, trailing bytes, oversized counts, oversized
text and metadata fields, malformed identifiers and text, invalid enumerations,
duplicate links, duplicate evidence keys, zero topology generations and
impossible generation relationships.

Recovery is conservative by construction:

* durable link records, generations, topology bindings, provenance and publisher
  fences are restored;
* live authority is not restored: no publisher incarnation survives a restart;
* persisted dynamic evidence is never restored as current — it is retained as an
  explicit disposition explaining why it is not current;
* every previously established link becomes REVALIDATION_REQUIRED, and a link
  that never carried a conclusion stays UNKNOWN;
* a fresh coordinator advances the coordinator epoch, so traffic from the
  previous epoch is rejected rather than replayed.

## Distributed publication

The runtime includes a real coordinator/worker transport: framed messages over
TCP with a version, bounded frame length, explicit message type, deterministic
codec, SHA-256 integrity field, stale epoch and stale boot checks, malformed
frame rejection and a clean connection lifecycle.

* A worker connects, performs a hello handshake and learns the coordinator
  epoch; it never presents a stale epoch by accident.
* Worker death is detected through the control connection closing. The
  coordinator fences the incarnations registered on that session.
* The coordinator writes its journal after every accepted mutation, so durable
  state is crash consistent without any wall clock checkpoint timer.

Distributed publication is implemented for Windows (Winsock) in this release.
On other platforms every transport entry point reports
`UNSUPPORTED_CAPABILITY`; the engine, evidence model, persistence and analysis
surfaces are portable.

## Resource limits

Every externally influenced resource is bounded and validated: identifier
length, detail text, opaque metadata, batch size, authority scope, link count,
evidence slots per link, publisher count, retained snapshots, state history,
persisted record count, frame payload size and concurrent sessions. Decoding uses
checked offsets and rejects absurd lengths before allocating.

## Build

Requirements: CMake 3.25 or newer, a C++20 compiler, and (on Windows) the
Windows SDK. The runtime has no third-party dependencies.

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Options: `LSF_BUILD_TESTS`, `LSF_BUILD_EXAMPLES`, `LSF_BUILD_TOOLS`,
`LSF_BUILD_BENCHMARKS`, `LSF_ENABLE_ASAN`, `LSF_WARNINGS_AS_ERRORS`.

First-party code is compiled with `/W4 /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wshadow -Werror` elsewhere. No warning is suppressed.

## Test

```
ctest --test-dir build/release --output-on-failure
```

The suite contains no timeouts: every test runs to completion, and a hang is a
defect to diagnose rather than to mask. Suites:

| Suite | Coverage |
| --- | --- |
| `lsf_test_identity_state` | identifiers, generations, digests, state model, full transition matrix |
| `lsf_test_engine` | binding, publication, idempotency, staleness, authority, topology, retirement, queries |
| `lsf_test_resolution` | conflicts, asymmetry, remote evidence, administrative gating, precedence, order independence |
| `lsf_test_persistence` | journal round trip, corruption matrix, truncation at every length, conservative recovery |
| `lsf_test_snapshot_scale` | snapshots, digest determinism, 10,000 and 100,000 link scale, synthetic backends |
| `lsf_test_concurrency` | deterministic race tests and seeded property tests |
| `lsf_test_adversarial` | resource limits, malformed input, backing references, real host evidence |
| `lsf_test_distributed` | frame and message codecs, real loopback sessions, protocol violations |
| `lsf_test_multiprocess` | real OS processes: worker death, permanent fencing, coordinator restart |

## Install and find_package

```
cmake --install build/release --prefix <prefix>
```

```cmake
find_package(LinkStateFabric CONFIG REQUIRED)
target_link_libraries(app PRIVATE SummonSoftwareLabs::LinkStateFabric)
```

An independent consumer lives in `validation/consumer` and is built from the
installed package only:

```
cmake -S validation/consumer -B build/consumer -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build/consumer
./build/consumer/lsf_consumer
```

## Examples

```
build/release/ex01_publish_up
build/release/ex02_down_transition
build/release/ex03_degraded
build/release/ex04_conflicting_evidence
build/release/ex05_stale_generation
build/release/ex06_stale_worker_boot
build/release/ex07_topology_invalidation
build/release/ex08_publisher_loss
build/release/ex09_persistence_recovery
```

## Tools

* `lsf-inspect journal <path>` validates a journal and prints its summary.
* `lsf-inspect records <path>` prints the durable link records of a journal.
* `lsf-inspect digest <path>` prints the journal file digest.
* `lsf-inspect host` enumerates real host visible link evidence.
* `lsf-inspect scenario <name>` runs a synthetic scenario and explains it.
* `lsf-coordinator` hosts the engine and serves the publication protocol.
* `lsf-worker` is a real publisher process.

## Benchmarks

```
build/release/lsf_benchmarks            # 10,000 and 100,000 links
build/release/lsf_benchmarks --small-only
```

Every benchmark asserts the outcome of each operation before timing it, so
throughput is only ever reported for completed work.

## REAL / SYNTHETIC / UNSUPPORTED validation

* **REAL**: host interface discovery through the operating system interface
  inventory (operational status, media connect state, negotiated speeds,
  administrative status, interface GUID, hardware description and physical
  medium type) on Windows. The runtime publishes this evidence as
  `EvidenceClass::REAL`. Every REAL observation describes exactly one endpoint:
  the local interface. No switch-side or fabric-wide truth is inferred from host
  local state.
* **SYNTHETIC**: generated switch, multi-switch, multi-site, optical-like,
  aggregate, logical and large-inventory fabrics, publisher death, stale replay,
  endpoint replacement, topology supersession and conflict scenarios. All
  synthetic evidence is labelled `EvidenceClass::SYNTHETIC` and never outranks
  real evidence.
* **UNSUPPORTED**: InfiniBand, RoCE, SmartNIC, DPU, vendor switch telemetry,
  multi-switch hardware fabrics and optical hardware are not present on the
  validation host. The runtime refuses to publish `EvidenceClass::UNSUPPORTED`
  evidence and reports the capability as unsupported rather than claiming a
  proof it does not have.

## Limitations

* The distributed transport is implemented and validated for Windows (Winsock);
  other platforms report `UNSUPPORTED_CAPABILITY` for every transport entry
  point.
* Host link-state discovery is implemented for Windows. On other platforms
  `discover_host_links()` reports `UNSUPPORTED`.
* The coordinator writes its journal after every accepted mutation. That is what
  makes durable state crash consistent without a wall clock checkpoint timer,
  and it bounds coordinator throughput accordingly.
* Fencing remembers a bounded number of fenced incarnations per publisher
  (`fence_retention_per_publisher`, default 64).
* No physical InfiniBand, RoCE, DPU or vendor switch telemetry was available for
  validation; those paths are exercised through the synthetic backend only.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
