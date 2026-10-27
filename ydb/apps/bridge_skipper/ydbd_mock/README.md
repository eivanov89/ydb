# ydbd_mock

An in-process Python mock of the public YDB Bridge API, used by the Skipper
acceptance tests in [`../acceptance`](../acceptance/README.md). It tests
client-visible behavior; it does not simulate real storage durability or
distconf consensus, and it cannot certify production failover.

`lib/testkit.LiveCluster` starts one simulated pile per name, each with several
nodes on their own loopback gRPC ports plus an HTTP control and peer API, all in
the test's event loop:

```python
cluster = LiveCluster(pile_names=("a", "b"), timings=tiny_timings(lifecycle_delay=0.05))
await cluster.start()
endpoints = {pile.name: ["grpc://" + node.endpoint for node in pile.nodes] for pile in cluster.topo.piles}
await cluster.kill("a")
```

The nodes serve `GetClusterState` (plain and with pile connectivity),
`UpdateClusterState` with `expected_generation`, `SelfCheck` with
`BRIDGE_GROUP` RED issues for blamed piles (and the BSC-unavailable fallback),
plus the lifecycle RPCs the mock needs internally. Piles commit snapshots with
a simplified protocol, apply YDB's follow-ups (promotion completion,
stage-2 synchronization, suspension to disconnection) and keep a per-node
request log. The HTTP control API injects faults (down, silence, partitions,
failpoints that hold or fail a method at a stage, replica overrides, timing
and policy changes).

Layout: `lib/` is the implementation (`cluster`, `peers`, `replica`,
`validator`, `followups`, `health`, `services`, `control_api`, `testkit` and
their helpers); `tests/` holds its unit and contract tests, including
`test_contracts`, `test_connectivity_report`, `test_health_modes`,
`test_suspend_quorum` and `test_proto_contract`.

Test target: `ydb/apps/bridge_skipper/ydbd_mock/tests`. The mock emits the
server's texts for bridge group parts, BLUE `Replication in progress` parts for
stage-2 piles, `PILE_*` issues for SYNCHRONIZED piles with unavailable nodes
and `HEALTHCHECK_STATUS` on request (control op `healthcheck_status`). Its
reason chains are shallower than the server's, and recorded real-YDB fixtures
remain the release gate for message texts.
