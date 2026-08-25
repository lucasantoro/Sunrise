# Bounded RX list decoder

The Pi-HAT comparator produces stable packet timing but an individual
interval can occasionally cross a run-length boundary. A single wrong cell
count displaces Manchester pairing until a later transition compensates it;
long packets can contain several independent slips.

The implementation is retained as an offline experiment, but it is disabled
in the deployed STM32 profile. After CRC/RS rejection it:

1. retains a bounded set of packet-wide candidates, prioritising runs of three or
   more cells and then proximity to a timing boundary;
2. evaluates the original, `n-1`, and `n+1` reconstruction for each candidate;
3. keeps a bounded beam of paths, with diversity across net cell-count
   displacement and edit depth;
4. ranks paths by Manchester bad-pair runs, total bad pairs, edit count, and
   timing confidence;
5. sends the best paths through the existing frame, RS, and CRC parser.

CRC remains the acceptance gate. The list decoder cannot forward a guessed
packet that does not pass the normal frame validation.

The beam workspace aliases the existing phase timing-margin buffer. It does
not add another packet-sized allocation or touch the DMA capture ring. The
choice maps retained on the stack occupy less than 1 KB.

Configuration is in `Core/Inc/openvlc_board.h`:

```c
#define OPENVLC_RX_LIST_RECOVERY   0u
#define OPENVLC_RX_LIST_CANDIDATES 16u
#define OPENVLC_RX_LIST_BEAM_WIDTH 16u
#define OPENVLC_RX_LIST_MAX_EDITS  6u
#define OPENVLC_RX_LIST_MAX_TRIALS 2u
```

The production default is `OPENVLC_RX_LIST_RECOVERY=0u`.

The production receiver does retain one much smaller CRC-gated fallback:
after a rejected frame it retries only the packet's closest run-length
boundary with the upper cell count. This costs at most one extra streaming
pass, does not allocate a list-decoder beam, and is configured with
`OPENVLC_RX_BOUNDARY_RETRY_MAX=1u`. The 32-good/32-fail capture population is
the regression set for this choice; increasing the retry count is not safe
without a new live deadline measurement.

At the saturated 1-Mbit/s profile the production receiver also uses one
streaming hypothesis and accepts at most two SFD locks per captured burst:

```c
#define OPENVLC_SFD_SYNC_HYPOTHESES_MAX 1u
#define OPENVLC_SFD_SYNC_LOCKS_MAX      2u
#define OPENVLC_RX_LOCAL_SYMBOL_RECOVERY 0u
```

The streaming pass decodes cells while traversing the edge burst and does not
perform the packet-sized symbol-cache repair scan. A damaged packet is
therefore abandoned before it can delay capture of subsequent packets.

## Offline validation

An unconstrained 32-candidate/64-path replay decodes the 2026-07-29 population
of 16 independently qualified 1-Mbit/s CRC failures 16/16. The successful CRC
path, however, ranks anywhere from 1 to 51. Consequently this result is not
safe to deploy on a saturated single-core receiver.

Live diagnostics measured 10-230 ms per affected burst while packets arrived
about every 8 ms. The DMA ring then accumulated and dropped edges, producing
secondary sync failures. Limiting the number of final trials avoids the
collapse but arbitrarily rejects captures whose valid path ranks later.

The list decoder must not be enabled live until path validation is incremental
or deadline-aware and worst-case execution stays below packet cadence.

## Live validation of the production decoder

After flashing, inspect the existing comparator diagnostic:

```text
COMP ... rp=<ring peak> rd=<ring drops> ... du=<last decode us> dm=<max decode us>
```

Run simplex first, then full duplex at the 1-Mbit/s profile. Check that
`dm` remains below packet cadence and that `rd`, `hwo`, and `ovf` do not
increase. Keep multi-capture enabled for the first run so any remaining error
can be replayed against the exact deployed decoder.
