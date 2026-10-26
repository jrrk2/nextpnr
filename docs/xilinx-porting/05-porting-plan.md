# 05 — Porting plan: nextpnr-xilinx → upstream himbaechel/xilinx

> **Status: draft for review.** Synthesises doc 01 (upstream architecture),
> doc 02 (fork inventory), doc 03 (algorithmic diff), doc 04 (gap list).
>
> **Governing philosophy: upstream first, fork fills the blanks.**
> Upstream nextpnr (and its himbaechel/xilinx uarch) is the *successor*
> of nextpnr-xilinx: it carries years of architecture and algorithm
> development (StrictLegaliser, control-set API, parallel_refine,
> timing_opt, placer_static, the router2 superset, the RelPtr chipdb
> model). Every decision below therefore **prefers the upstream
> solution**; nextpnr-xilinx material is imported only where upstream has
> a *blank* (missing legality checks, missing packers, missing FASM
> correctness fixes, missing validation) — never as a replacement for
> upstream code that exists. Where upstream already has an equivalent
> (e.g. `--report`, himbaechel framework, control-set checks), the fork's
> version is explicitly *not* ported.

---

## 0. Strategy & principles

1. **Prefer upstream solutions.** Upstream nextpnr is the successor
   architecture of nextpnr-xilinx: years of architecture/algorithmic
   development live there (RelPtr chipdb model, StrictLegaliser, the
   control-set API, `parallel_refine`, `timing_opt`, `placer_static`,
   the router2 superset). Every decision in this plan therefore keeps
   the upstream implementation wherever one exists, and imports fork
   material **only to fill blanks**. Concretely: keep the upstream
   `StrictLegaliser` (same greedy random-walk family as the fork's,
   refactored with bel buckets and control-set pre-allocation) but
   **also port the fork's termination guarantees** (fail-fast,
   deterministic fallback, eviction-as-last-resort) since the same walk
   family retains their livelock risk (doc 03 corrigendum). Do not port
   fork placer/router code wholesale — port *checks, data, and
   robustness guarantees*, and skip anything upstream already has (e.g.
   `--report`, the himbaechel framework, control-set checks).
2. **Legality first.** The five high-risk missing checks (OUTMUX budget,
   cross-position carry→FF rejection, FF1-uses-X+5FF co-pack, SRL cascade
   placement, CFG preplacement — doc 03 §4) gate everything else: without
   them, upstream's legaliser will re-create the exact illegal slices the
   fork's bugfixes reject.
3. **Port at the fork's own maturity order.** The fork's 0.9.2→0.9.3 digest
   shows the proven fix sequence: placement legality → FASM correctness →
   primitive modes → constraints → CI gate.
4. **Validate with golden bitstreams** (fork CI's normalised-hash method) at
   every work package, not at the end.
5. **No code changes, no commits** until these documents are reviewed and
   the user approves implementation work (per repo policy).
6. **xc7 only — UltraScale(+) porting is explicitly EXCLUDED.** This plan
   ports Xilinx 7-series (xc7) support only: artix7, kintex7, zynq7,
   spartan7, virtex7. The fork's UltraScale/UltraScale+ work — the
   `xcup` RapidWright flow, E2/E3/E4 packers (CARRY8, RAMB36E2, DSP48E2,
   URAM288, ISERDESE3/OSERDESE3, …), xcup device databases, and the
   DCP/RapidWright export tooling — is **out of scope by decision**.
   UltraScale-related observations in docs 01–04 are recorded for
   completeness only; they are not inputs to this plan and must not be
   implemented under it.

## 1. Work packages (ordered)

### WP0 — Device-list & build foundation (small, no risk) — ✅ IMPLEMENTED (pending build validation)
- ✅ Added `xc7k70t/160t/325t/420t/480t`, `xc7z030/045/100`, `xc7vx485t`
  to `ALL_HIMBAECHEL_XILINX_DEVICES` (`himbaechel/uarch/xilinx/CMakeLists.txt:32`);
  `xc7z035` deferred — no die dir in openXC7 prjxray-db (part dirs only).
- ✅ Virtex-7 plumbing: CMake `xc7v` → virtex7 mapping; gen script virtex7
  metadata selection + artix7 timings fallback (`xilinx_gen.py`); device
  regex extended to `xc7vx\d+t?` (`xilinx.cc:81`).
- ✅ Meta sync: submodule switched gatecat → **openXC7/nextpnr-xilinx-meta**
  master (`a4af910`, adds virtex7 site types + kintex7 PCIE_2_1;
  `.gitmodules` URL updated).
- ⚠ a35t: openXC7 prjxray-db (`ab1fc60`) has a35t *part* dirs but **no a35t
  die dir** — the a35t→a50t die alias (`xilinx.cc:86`) therefore stays
  (chipdbs are die-level); correcting the earlier assumption that the fork
  had a real a35t die.
- **Validate**: chipdb gen + build for xc7a50t/xc7k325t/xc7vx485t/xc7z045
  (in progress); then `arty-a35` blinky + archcheck.

### WP1 — Port the legality engine (core, must be first) — ✅ IMPLEMENTED (validation in progress)
Ported into `himbaechel/uarch/xilinx/xilinx_place.cc` (re-expressed over
`LogicTileStatus`/`XilinxCellTags`; no `constr_*` walks exist upstream):
1. ✅ Per-position OUTMUX (site-exit) budget — fork `arch_place.cc:424-506`.
2. ✅ Cross-position carry→FF flat rejection + 5FF-fed-by-carry rejection —
   fork `arch_place.cc:731-745, 766-774`; also added CO as a direct-feed
   shape for the main FF (missing upstream).
3. ✅ Main-FF-via-X-bypass + 5FF co-pack rejection — fork `arch_place.cc:789-804`.
4. ✅ Strengthened 5LUT A6/O6 gate (connected-input count + A6 check,
   memory/SRL exempt) and made the SLICEM-only guard unconditional — fork
   `arch_place.cc:368-423, 626-656`.
5. ✅ Per-candidate gate: satisfied by making checks 1/2/4 unconditional —
   they now run on every `isBelLocationValid` call even when the tile's
   dirty cache is clean, so no separate `isValidBelForCell` hook is needed.
6. ✅ Carry-chain continuation + CO/OUTMUX contention (CO3 spine exception,
   CO0..2 always claim) — fork `arch_place.cc:477-491, 859-887`.
7. ✅ SRL16E pair exemption in the 6LUT+5LUT coexistence/shared-input checks
   (fork `arch_place.cc:548-583`).
- ⏭ Not ported (hybrid Vivado-import flow only, out of scope): frozen-tile
  fast path, imported-slot/BEL exemptions, `lut_routethru_feed` exemption,
  `NEXTPNR_ALLOW_CO_5FF_CONTENTION` env, `dbg_validity_runtime` tracing.
- **Validate**: litex-ddr-arty-s7 (the design that exposed the fork's carry
  bugs) × seeds 1–4 on xc7s50 (running); blinky/arty-a35 regression passed;
  unit tests deferred to WP9.

### WP2 — FASM correctness cluster (independent of WP1, high value) — 🔄 in progress
Port in commit-sized units, each with a bitstream-hash check:
1. ✅ Phantom-BUFGCTRL guard (fork `fasm.cc:122`) — committed.
2. 🔄 HP-bank IO glue: of the five fork commits, **two apply upstream**
   (✅ `6f33adf0` SSTL15/135 SLEW.SLOW group skip; ✅ `e4a261ce` IN_ONLY
   partner-output gate). The other three guard emissions upstream does not
   yet have (`d6b7f64d` IBUF_HP_BANK_GLUE, `70a5952c` cross-site
   SLEW.SLOW defaults, `c2e50b99` diff-input SLEW.SLOW) — deferred until
   the corresponding HP-glue emissions are ported, not dropped.
3. SDP BRAM opposite-port width + 36-wide marker + ZINV_REGCLK*
   (`f1c77134`, `11f9b694`, `1b7d51b9`, `e71acda2`).
4. ✅ OSERDES/ILOGIC bits: `IS_CLKDIV_INVERTED`, `TRISTATE_WIDTH.W4`,
   `IFF.INV_OCLK` (`b9ed05a2`, `c05f0d05`).
5. ✅ PLL LKTABLE/TABLE from PLL-specific tables (`e33b5f1a`,
   `74357a79`) — 63-entry tables indexed by CLKFBOUT_MULT, BANDWIDTH=LOW
   table, harvested from Vivado goldens.
6. ✅ BUFR_DIVIDE on placed BUFR (`0b914578`) + BUFR packing/placement +
   HCLK_IOI pip-filter fix (see WP4.4 note).
7. Run-identity FASM header (`7037c948`) — trivial, do first as a warm-up.
- **Validate**: fork CI's normalised bitstream-hash comparison on
  arty-s7/arty/kintex7 demo projects (needs WP9 CI scaffolding or local
  equivalents).

### WP3 — Primitive packer gaps (feature-level)
1. ⏭ **MUXF9 on xc7**: NOT a gap — the fork also hard-errors on xc7
   (`pack.cc:648`); its SELMUX2_1 mapping covers F7/F8 only (F9MUX is
   xcup-only). Dropped from the plan (earlier doc-02 claim corrected).
2. ✅ **BUFH** (non-CE) cell + packing: BUFH/BUFHCE → `BUFHCE_BUFHCE` with
   CE tied active in `prepare_clocking`, + `try_preplace` in
   `preplace_clocking`; constids `X(BUFH)`, `X(BUFHCE_BUFHCE)` added
   (upstream previously packed NEITHER BUFH nor BUFHCE).
3. ⏭ **Dist-RAM**: NOT a gap (WP3.3 verified) — the fork's three fixes are
   already present upstream (scalar A0..A6 special-case, m256 mux-tree
   zoffset 0), and RAM512X1S/D / RAM32M16 / RAM64M8 / RAM64X2S /
   RAM64X8SW are declared-but-unpacked on BOTH sides. Dropped from the
   plan (earlier doc-02 claim corrected).
4. ✅ **SRL**: Q31 (MC31) support + cascade placement rules
   (`constrain_srl_cascades`) with cluster-based D-C-B-A grouping.
5. ✅ **IDDR**: `DDR_CLK_EDGE=SAME_EDGE_PIPELINED` + 4-IFF-flop init
   (`9a6a7e3b`, `d455ae52`); routethru SRTYPE fixes (`f77907ac`,
   `16accf3b`) inapplicable upstream (pp_config already omits SRTYPE).
6. ✅ **ISERDES/OSERDES**: OFB loopback placement (OSERDESE2->ISERDESE2 pair
   bound to a free site pair); master/slave pairing was already upstream. 7. ✅ **IDELAYCTRL** no-delay warning already upstream (pack_io.cc).
7. **IDELAYCTRL** no-delay → warning (`06769c05`).
8. ✅ **IBUFGDS** alias of IBUFDS (`55c3bc87`): constid `X(IBUFGDS)` +
   `cells.cc` port list + `pins.cc` toplevel + `is_diff_ibuf` test + HR/HP
   rule maps. OBUFDS swapped-pin diagnostic (`0ebf6394`) pending.
- **Validate**: fork `primitive-tests/` repo designs + demo projects;
  targeted synth of each primitive via `synth_xilinx`.

### WP4 — Placement & routing decisions (follows WP1) — 🔄 partially done
1. ⏭ **`relocate_carry_o_fabric` decision: DEFERRED.** With the WP1 legality
   checks in place, the upstream legaliser+router absorb the common O+CO
   dual-fanout cases; litex-ddr-arty-s7 (the fork's carry regression design)
   passes ×4 seeds. The netlist-level pass will be ported only if a concrete
   failing case appears.
2. 🔄 **Delay model**: ported the fork's tuned formulas into upstream
   `estimateDelay`/`predictDelay` (`xilinx.cc`, replacing the coarse TODO
   version): the fork's 30/10·18, 60/20·6 base + ×3/2 xc7 factor, sink-loc
   +1000, PINFEED/LOCAL/PINBOUNCE/CLE_OUTPUT discounts, and predictDelay's
   same-tile 0/150/700 (FF2 penalty) model.  Kept upstream's coordinate
   resolution (source/sink locs + long-wire pips) instead of the fork's
   inter_x/inter_y site lookups (himbaechel chipdb has no per-site inter
   coords).  Build/validation pending WP3.4 subagent (build-dir owner).
3. ✅ **router2 config**: new `HimbaechelAPI::configureRouter2()` hook (set in
   `Arch::route()`); XilinxImpl sets `bb_margin_{x,y}=4`,
   `backwards_max_iter=200`, `perf_profile=true`.
4. ⏭ **routeVcc + clock-backbone ordering**: assessed NOT needed upstream —
   router2 routes constant nets itself and `route_clocks` pre-binds clock
   nets LOCKED before router2 runs, so the fork's post-router Vcc fill /
   "Vcc floods the clock backbone" failure mode does not apply.  Deferred
   unless a concrete failure appears (the HCLK_IOI clock-entry pip filter
   fix in WP2.6 was the one DB-side piece that mattered).
5. ✅ **Final timing analysis after router2** (`7ea51730`): added generically
   in `Arch::route()` — router2 previously ran no final analysis (router1
   does); verified post-route fmax report appears.
6. Keep `fixupPlacement` as belt-and-braces only if WP1 proves insufficient;
   port the STRENGTH_USER-vs-STRONG skip distinction (doc 03 §6 risk 6).

### WP5 — XDC parser improvements (small, user-visible) — 🔄 code done, build pending
From coordinator note `drafts/00c`:
1. ✅ `name[0]` de-busing retry in `get_cells` + `get_nets` (`b257be4d`).
2. ✅ Silent non-design targets unless `--verbose` + summary line
   (`3da43687`, `555d326c`) — misses now itemise only in verbose mode.
3. ✅ Virtual-clock skip guard and "constraint NOT applied" warning.
4. ⏭ BEL-attr-unknown-tile non-fatal (`8399469c`) — verify upstream
   behaviour separately (placement-time, not parser).
5. ✅ **set_multicycle_path -setup**: upstream SDC has no multicycle support
   at all (create_clock/set_false_path only), so the fork's attribute
   mechanism was ported (XDC parser + timing-walk relaxation).

### WP6 — Config/misc IP preplacement (easy after WP1)
BSCANE2, DNA_PORT, EFUSE_USR, ICAPE2, FRAME_ECCE2, STARTUPE2,
USR_ACCESSE2, DCIRESET — single-site preplacement per fork
`pack_io_xc7.cc:1232` (`d42d6c9b`) + FASM emission. Depends on the
preplacement mechanism from WP1.

BSCANE2 is the exception to the fork's rule: preplacing it on the first free
BSCAN bel is wrong, because USER<n> is served by site BSCAN_X0Y<n-1> and a
BSCAN site's pins *are* that chain's SEL/CAPTURE/SHIFT/DRCK wires.  A design
with more than one instance got its second one placed on another chain's
site and answered the wrong user register.  The port binds each instance to
the site its JTAG_CHAIN selects instead (`bscan_bels_by_chain`), reports a
site already taken, and leaves a bel the design pinned itself alone.

### WP7 — GT transceivers (GTPE2/GTXE2) (larger) — ✅ DONE
Ported `pack_gt_xc7.cc` (pack_gt/constrain_gt/constrain_ibufds_gt_site via
bindBel + SiteIndex instead of the fork's BEL-attr strings) and the five
FASM writers (write_ibufds_gte2/write_gtp_pll/write_gtp_channel/
write_gtx_pll/write_gtx_channel, 1.4k lines).  GT pads (OPAD/IPAD) are
exempted from the IO-buffer xform, the IOSTANDARD check, and the IO FASM
path.  The fork's GT-clock template route (`NEXTPNR_GT_CLK_BODGE`) is a
virtex7-specific bodge and is NOT ported (noted).  PCIE_2_1 writer and
pack plumbing ported as a follow-up (see below).
Validated: GTPE2_CHANNEL+GTPE2_COMMON design (gtp-gtx-tests/gtp_channel)
P&R on xc7a200tfbg484-1 completes with 171 GTPE2 FASM features emitted;
blinky + litex-ddr-arty-s7 regressions and all 5 unit tests pass.

### WP7b — PCIE_2_1 hard block — ✅ DONE
Ported the 640-line `write_pcie_2_1` FASM writer (PG054 PCIe config
space: BARs, AER/MSI/MSI-X/PM caps, link control, pipe/misc registers)
plus the pack-time plumbing mirroring PS7: `PCIE_2_1`→`PCIE_2_1_PCIE_2_1`
retype in `pack_io.cc` and `preplace_unique` binding on the unique PCIE
site in `pack_clocking.cc`.
Validated end-to-end at bitstream level on xc7k325tffg676-1: synthetic
PCIE_2_1 design places at PCIE_BOT_X189Y167 and emits 190 features; all
370 set bits are legal in the openXC7 kintex7 segbits; prjxray-alt
fasm2frames + xc7frames2bit produce a bitstream; bit2fasm roundtrip
reproduces all 123 non-zero features bit-for-bit (0 mismatches).
Regression battery (blinky-arty, qmtech blinky, litex-ddr-arty-s7) and
archcheck pass.

### WP8 — EXCLUDED: UltraScale / UltraScale+ porting (xc7-only decision)

**Explicitly out of scope.** The fork's xcup flow (RapidWright BBA
export, E2/E3/E4 packers, xcup device databases, DCP export,
UltraScale+ FASM writers) will **not** be ported under this plan
(principle 6). No UltraScale work package exists; anything
UltraScale-related found in the fork is ignored for the purposes of
this effort. If UltraScale support is ever desired, it must be planned
as a separate project with its own documents.

### WP9 — Validation & CI (runs alongside all packages) — ✅ DONE
- ✅ demos regression gate workflow (`.github/workflows/demos.yml`): builds
  the PR's himbaechel/xilinx binary + chipdbs from openXC7 prjxray-db
  (pinned revision), builds xc7frames2bit from openXC7/prjxray (pinned),
  runs the five-demo subset via `.github/scripts/nextpnr-xilinx-shim.sh`
  with a pinned `--seed`, and **fails the gate on sha256 mismatch** of the
  produced `.frames` against `.github/goldens/<project>.sha256`.  Bitstreams
  are uploaded as artifacts.  (WP9.3 re-golden path documented in the
  workflow's failure message.)
- ✅ part-form / bare-die device names accepted (CI shim derives the device
  from the chipdb filename).
- ✅ xilinx gtest coverage: `uarch/xilinx/tests/pack_test.cc` (10 packer
  tests: STARTUPE2 cfg packing + preplacement, BSCANE2 packing bound to the
  site its JTAG_CHAIN selects (that site, two chains in one design, a
  duplicate chain rejected), BUFH/BUFHCE, BUFR, IBUFGDS alias, SRL cascade
  pair/off-slice clustering, PCIE_2_1 retype + preplacement on k325t), wired
  via TEST_SOURCES.
- ✅ archcheck fully green on all seven devices (`--test` on the main
  binary — note the gtest binary ignores `--test`).  Fixes: non-primary
  variant bels get a `~<variant>` name suffix; site pips are deduped per
  (src,dst); tile-wide bel z is made bijective (colliding variant bels get
  a fresh z, and BRAM-tile inversion bels start at z 12 so the semantic
  0..11 slots in `BRAMTileStatus` stay reserved for the BRAM cells); the
  BRAM tile status folds a re-allocated variant twin back onto its primary
  slot.
1. ~~Add xilinx gtest coverage~~ — done, see above.
2. ~~Port the fork's per-PR demos gate~~ — done: PR binary + chipdbs
   (xc7s50, xc7a35t, xc7k325t), demo projects, `.frames` golden hashes,
   `.bit` upload.
3. Golden set: generated with THIS repo's himbaechel implementation at the
   pinned prjxray-db revision (fork goldens cannot match — himbaechel
   placements differ); re-golden after intentional FASM changes with review.
4. `archcheck` stays as the fast gate.

## 2. Dependency graph

```
WP0 ──┬── WP1 ──┬── WP3 (SRL cascades, carry modes)
      │         ├── WP4 (carry-O decision, delay model, router2 cfg, Vcc/clk ordering)
      │         └── WP6 (config IP preplacement)
      ├── WP2 (FASM)        ← independent, can run in parallel with WP1
      ├── WP5 (XDC)         ← independent
      ├── WP7 (GT)          ← needs WP0 kintex7/virtex7 devices
      └── WP9 (CI/tests)    ← starts immediately, gates every WP
WP8 (UltraScale/xcup) — EXCLUDED by decision, not part of this graph.
```

## 3. Suggested execution order (fastest path to a releasable state)

1. WP9 scaffolding + WP2.7 (run-identity header) — CI warm-up.
2. WP0 (device list) → WP1 (legality) → WP4.3/4.4 (router2 cfg + Vcc/clock
   ordering) → WP2 (FASM cluster).
3. WP3 → WP5 → WP6 → WP7.

This mirrors the fork's own 0.9.2→0.9.3 sequence (legality fixes first,
then FASM, then primitive modes), which is the empirically proven order.

## 4. Risks & mitigations (from doc 03 §6)

| Risk | Mitigation |
|---|---|
| `constr_*`/ClusterId API mismatch silently no-ops ported heuristics | Re-express via ClusterId/BelBucketId; add asserts; review each port |
| Upstream legaliser produces illegal slices without WP1 checks | WP1 first; golden-bitstream diff catches bit corruption |
| Coarse upstream delay model degrades timing QoR | WP4.2 (fork delay model) before enabling timing-driven flows |
| Fixed-routes/frozen-import features not ported | Accept the hybrid-flow features as out of scope (doc 03 §5.8) unless user needs them |
| Fork repo advances mid-port (it gained 8 commits while analysing) | Re-diff at each milestone; tag 0.9.3 is the current reference |
| UltraScale scope creep | Excluded by principle 6: xcup/E2/E3/E4 content is ignored; review gates reject UltraScale changes |

## 5. Definition of done

- All doc 04 gap rows resolved (ported or explicitly deferred with reason),
  **excluding UltraScale/xcup rows, which are out of scope by decision**.
- Doc 03 §4 must-port checks present + unit-tested in upstream tree.
- Demo-projects golden-bitstream hashes match (post re-golden review) on
  xc7s50, xc7a35t, xc7k325t.
- Upstream CI (archcheck + demos gate) green; non-xilinx arches unaffected.
- No UltraScale(+) code, device databases, or tooling has been added.
- Documents 01–04 updated to reflect the final state.

---

## 6. Re-diff against the fork's current main (2026-09)

The plan above took the fork at tag `0.9.3`. Re-diffing at the fork's main
(`3fd78784`, tag `0.9.6`) gives `git diff --stat 0.9.3..main -- xilinx/` = 14
files, +1477/−96, and this tree was moved to `main` (`152860f8`, 136 commits
past the then-checked-out `himbaechel-xilinx-porting`).

**How the remaining gap was measured.** `demo-projects/regression/` already
holds one case per fixed fork bug, each asserting a property of the FASM or of
the routed netlist. Running those cases against a build of this tree through
the fork's CLI shim (`.github/scripts/nextpnr-xilinx-shim.sh`) turns them into
the porting work list, which is a stronger instrument than reading diffs:
**9 of the 11 cases pass** as of `49372f4a`.

**Ported and verified by those cases** (commits `b40ce4af`, `49372f4a`):

- `RAM64X1S`: the arm passed the RAM cell's `O` to `create_dram_lut()` without
  disconnecting it, aborting the packer on any 64-deep single-port memory
  (`lutram-ram64x1s`, `lutram-clkinv`);
- the same arm tested `(z - 1) < 0` for "site is full", retiring every slice
  one LUT early — three slices of three LUT-RAMs instead of two of four
  (`lutram-ram64x1s`);
- `write_ffs_config()` took the half-slice clock inversion from the flipflops
  only, so an inverted LUT-RAM write clock emitted `NOCLKINV`
  (`lutram-clkinv`);
- `INIT` defaulted to 0 for every FF, but FDSE/FDPE default to 1, and a
  present-but-undefined `'x'` INIT read as 0 (`fdse-fdpe-undefined-init`);
- LUT6_2 was never split, so its `O5` half had no bel pin
  (`lut_shared_pin`);
- the LUT-pair legaliser put the `X_ORIG_PORT` separator after each element,
  producing `"I2I0 "` (`lut_shared_pin`).

`.github/workflows/regression.yml` runs the suite in this repo: the ported
cases block, the outstanding ones run `continue-on-error` naming their issue.

**Still to port**, each with the failure it produces:

| item | evidence |
|---|---|
| ~~`const-holdout` (#184)~~ | **ported** — the routing was never missing: the constant router had already reached all 192 RAM32M address site pins. `check_const_pins.py` read them as unreached because this tree serialised `NEXTPNR_BEL` and `ROUTING` in himbaechel's `<tile>/<site>.<bel>` dialect while the checker parses nextpnr-xilinx's `<site>/<bel>` and `SITEWIRE/<site>/<pin>`. A new `HimbaechelAPI::postRouteArchInfo()` hook, called after `archInfoToAttributes()`, lets the xilinx uarch emit the dialect its tooling reads. An earlier packer-level attempt (a LUT driver for the tied pins) failed the same check for the same reason and was reverted |
| ~~SRL16E/SRLC32E `INIT` (`1193ed03`)~~ | **ported** — `get_lut_init()` writes the SRL's own INIT directly (each bit k at LUT INIT bits 2k and 2k+1, with the SRL16E 6LUT position narrowed to [32:64)); case `srl-init` |
| ~~X_ORIG_PORT unknown-name guard (`7cfd1e90`)~~ | **ported** — `get_lut_init()` uses `find()` + `log_error()` instead of `operator[]`, so an unknown logical-input name stops the flow instead of encoding as I0; case `xorigport-unknown-name` |
| ~~WEMUX half-tile consistency (`ccfae5ae`)~~ | **ported** — `xc7_logic_tile_valid()` now stores each memory/SRL LUT's WE net and rejects two cells in a SLICEM half with different WE; case `srl-wemux` |
| ~~duplicate-package-pin warning (`9efb656d`)~~ | **ported** — `pack_io()` warns when two pads are constrained to one package pin, in the user's names; case `dup-package-pin` (expected-fail: the design is still unplaceable) |
| ~~BUFIO `IN_USE` (`c52d41b6`)~~ | **ported** — `write_bufio` + the BUFIO packer (`#157`) + pad-fed BUFIO preplacement were already in (`bufio-in-use`); the case was blocked on the pad→BUFIO `I2IOCLK` segbits, which landed in prjxray-db 77e52f10 (`f2a469b8`). The pin is now bumped (see below), the chipdb regenerated, and `bufio-in-use` routes and emits `BUFIO_Y*.IN_USE` |
| ~~regional-buffer sink regions (`20dc8309`)~~ | **ported** — `constrain_regional_clock_sinks()` walks the routing graph from the placed buffer's `O` wire (collecting only `CLK` bel pins) to derive the clock-region rectangle, then constrains each sink's cluster root to it; case `bufr-sink-region` (placement-level: the BUFR I pin cannot route until the prjxray-db bump) |
| ~~pad-fed BUFR dedicated site (`7c4f00df`, `f440166f`)~~ | **ported** — `constrain_bufios()` binds a pad-fed BUFIO/BUFR to the one site its pad's `I2IOCLK` leg reaches (`find_bel_with_short_route` from the input buffer's bel), replacing the old `try_preplace` BUFIO path; case `bufr-pad-site` (placement-level) |
| ~~BUFH/BUFR/BUFIO/BUFMR + MMCM secondary-output clock propagation (`13d88882`)~~ | **ported** — `generate_constraints()` now carries a constraint through BUFGCTRL I0/I1, BUFHCE, BUFR (divided by `BUFR_DIVIDE`), BUFIO and BUFMRCE, and derives the MMCM's CLKOUT0B..3B and CLKFBOUT(B); case `bufh-clock-constraint` (BUFH is the routable representative; BUFR/BUFIO share the path but need the prjxray-db bump to route) |
| ~~prjxray-db pin~~ | **bumped** — CI now pins `77e52f10` (was `ab1fc60c`); blast-radius checked: `bufio-in-use` newly routes; of the 14 blocking cases only `clock-srcc-bufg` (2 lines), `bufg-fabric-driven` (~15) and `srl-init` (5) shift, all benign routing/placement deltas from the HCLK_IOI3 routing-graph change (`I2IOCLK` DMUX ppips added, `RCLK2IO→CK_BUFRCLK` moved from always-on to segbits) |

`dsp-const-only-pins` (#159) is a blocking case since #66, which ties the
DSP48E1's constant-only inputs in the tile as Vivado writes them; the fork's
PR #159 is still unmerged.

