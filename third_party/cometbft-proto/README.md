# Vendored ABCI protobuf schemas

Exact copies (ADR-0027), checked by `tests/abci/proto_checksums_test` against `SHA256SUMS`:

| Files | Source | Licence |
|---|---|---|
| `tendermint/abci/types.proto`, `tendermint/types/{params,validator}.proto`, `tendermint/crypto/{keys,proof}.proto` | github.com/cometbft/cometbft tag **v0.40.0** (commit `0880b4d378f347ab16e54ec677ff50d803f37d62`), `proto/` | Apache-2.0 (`LICENSE-cometbft`) |
| `gogoproto/gogo.proto` | github.com/cosmos/gogoproto **v1.7.2** (the version in CometBFT's `go.mod`, verified by its `go.sum`) | BSD-3-Clause (`LICENSE-gogoproto`) |

`google/protobuf/{timestamp,duration,descriptor}.proto` come from the pinned protobuf release.
Do not edit these files; update them only together with the CometBFT version (new ADR).
