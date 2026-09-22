/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2023  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <boost/algorithm/string.hpp>
#include <regex>

#include "extra_data.h"
#include "himbaechel_api.h"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include "xilinx.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

// #define DEBUG_VALIDITY
#ifdef DEBUG_VALIDITY
#define DBG() log_info("invalid: %s %d\n", __FILE__, __LINE__)
#else
#define DBG()
#endif

bool XilinxImpl::xc7_logic_tile_valid(IdString tile_type, const LogicTileStatus &lts) const
{
    bool is_slicem = (tile_type == id_CLBLM_L) || (tile_type == id_CLBLM_R);
    bool tile_is_memory = false;
    if (lts.cells[(3 << 4) | BEL_6LUT] != nullptr && get_tags(lts.cells[(3 << 4) | BEL_6LUT])->lut.is_memory)
        tile_is_memory = true;
    bool small_memory = false;
    if (lts.cells[(3 << 4) | BEL_5LUT] != nullptr && get_tags(lts.cells[(3 << 4) | BEL_5LUT])->lut.is_memory)
        small_memory = true;
    NetInfo *wclk = nullptr;

    // SLICEM-only guard, run UNCONDITIONALLY (not behind the per-eight dirty
    // cache in the loop below).  A distributed-RAM / SRL LUT (is_memory /
    // is_srl) is legal only in a SLICEM.  The dirty-gated loop leaves this
    // unchecked whenever a tile is validated while clean, so the placer /
    // legaliser can strand a memory/SRL LUT in a SLICEL -- surfacing much
    // later as an unroutable "Pin 'DI1'/'WE' of bel ... has no associated
    // wire".  A cheap O(16) scan here closes that hole for both the 6LUT
    // and 5LUT bels.  (Port of nextpnr-xilinx arch_place.cc.)
    if (!is_slicem) {
        for (int i = 0; i < 8; i++) {
            auto l6 = get_tags(lts.cells[(i << 4) | BEL_6LUT]);
            auto l5 = get_tags(lts.cells[(i << 4) | BEL_5LUT]);
            bool l6_needs_slicem = l6 != nullptr && (l6->lut.is_memory || l6->lut.is_srl);
            bool l5_needs_slicem = l5 != nullptr && (l5->lut.is_memory || l5->lut.is_srl);
            if (l6_needs_slicem || l5_needs_slicem)
                return false;
        }
    }
    // A 5LUT bel has neither an A6 input nor a second output; a cell there
    // must not use either.  Enforce UNCONDITIONALLY -- the dirty-gated loop
    // below caches past this, letting the legaliser strand a 6-input LUT on
    // a 5LUT ("No wire found for port A6/O6").  Memory/SRL LUTs are exempt:
    // they legitimately draw many ports (address + DI + WE) and the SLICEM
    // guard above already constrains them.  Reject only what the 5LUT bel
    // PHYSICALLY cannot provide: a 6th input or a second output.  A single-
    // output cell whose port happens to be NAMED O6 is NOT invalid here:
    // that is the documented pre-fixup state of every carry DI feed-through
    // (pack_carry constrains them to the 5LUT bel with the output still
    // called O6, and fixup_placement() renames it to O5 after placement).
    //  (Port of nextpnr-xilinx arch_place.cc.)
    for (int i = 0; i < 8; i++) {
        CellInfo *l5c = lts.cells[(i << 4) | BEL_5LUT];
        if (l5c == nullptr)
            continue;
        auto l5 = get_tags(l5c);
        if (l5 == nullptr)
            continue;
        bool is_memory_or_srl = l5->lut.is_memory || l5->lut.is_srl;
        if (is_memory_or_srl)
            continue;
        bool needs_a6_or_second_output =
                l5->lut.input_count > 5 || l5->lut.output_count == 2 || l5c->getPort(id_A6) != nullptr;
        if (needs_a6_or_second_output)
            return false;
    }
    // Per-position site-exit (OUTMUX) budget, run UNCONDITIONALLY.  Each
    // letter position has exactly ONE selectable output pin ([A-D]MUX: O5,
    // XOR = carry O, CY = carry CO, A5Q, ...) besides the dedicated O6 pin
    // and the FF's Q.  The packer/legaliser could co-locate a 5LUT with
    // fabric consumers, a carry whose O feeds an off-position FF (or
    // fabric), and more -- each needing that one pin.  The router then
    // inherits an unroutable site: "Failed to route arc ... CARRY4_O3 to
    // AFFMUX_OUT".  Reject any position with more than one OUTMUX claimant
    // so the legaliser keeps searching instead.
    //  (Port of nextpnr-xilinx arch_place.cc.)
    for (int h = 0; h < 2; h++) {
        CellInfo *cy = lts.cells[(h << 6) | BEL_CARRY4];
        auto cy_tags = get_tags(cy);
        for (int k = 0; k < 4; k++) {
            int eighth = h * 4 + k;
            CellInfo *l6c = lts.cells[(eighth << 4) | BEL_6LUT];
            CellInfo *l5c = lts.cells[(eighth << 4) | BEL_5LUT];
            CellInfo *ff1c = lts.cells[(eighth << 4) | BEL_FF];
            CellInfo *ff2c = lts.cells[(eighth << 4) | BEL_FF2];
            auto l5_tags = get_tags(l5c);
            auto ff1_tags = get_tags(ff1c), ff2_tags = get_tags(ff2c);
            // in-position sinks that do NOT need the OUTMUX
            auto is_local_sink = [&](const PortRef &usr, NetInfo *net) {
                bool feeds_main_ff = ff1c != nullptr && usr.cell == ff1c && ff1_tags->ff.d == net; // via xFFMUX
                bool feeds_5ff = ff2c != nullptr && usr.cell == ff2c && ff2_tags->ff.d == net;     // via xFF5MUX
                bool feeds_carry = cy != nullptr && usr.cell == cy;                                 // S/DI/CIN
                bool feeds_lut_in_position = (l6c != nullptr && usr.cell == l6c) ||
                                             (l5c != nullptr && usr.cell == l5c); // routethru cases
                return feeds_main_ff || feeds_5ff || feeds_carry || feeds_lut_in_position;
            };
            auto has_external_user = [&](NetInfo *net) {
                if (net == nullptr)
                    return false;
                for (auto &usr : net->users)
                    if (!is_local_sink(usr, net))
                        return true;
                return false;
            };
            int claims = 0;
            // O5 of a used 5LUT (O6 has its own pin; DI feed-throughs and
            // in-position FF feeds are local)
            bool is_plain_5lut = l5c != nullptr && l5_tags != nullptr && !l5_tags->lut.only_drives_carry &&
                                 !l5_tags->lut.is_memory && !l5_tags->lut.is_srl;
            bool o5_leaves_position = is_plain_5lut && has_external_user(l5_tags->lut.output_sigs[0]);
            if (o5_leaves_position)
                claims++;
            bool has_carry = cy != nullptr && cy_tags != nullptr;
            if (has_carry) {
                // carry sum O_k beyond the in-position FF
                if (has_external_user(cy_tags->carry.out_sigs[k]))
                    claims++;
                // carry CO_k beyond the chain continuation
                NetInfo *co = cy_tags->carry.cout_sigs[k];
                if (co != nullptr) { // an unused CO claims nothing
                    for (auto &usr : co->users) {
                        // the chain continuation (k==3) uses the dedicated COUT
                        const bool is_chain_continuation =
                                usr.cell != nullptr && usr.cell->type == id_CARRY4 && usr.port == id_CIN;
                        if (is_chain_continuation)
                            continue;
                        if (is_local_sink(usr, co))
                            continue;
                        claims++;
                        break;
                    }
                }
            }
            // 5FF Q has no dedicated pin: fabric consumers go through OUTMUX
            if (ff2c != nullptr) {
                NetInfo *q2 = ff2c->getPort(id_Q);
                bool ff5_q_is_used = q2 != nullptr && !q2->users.empty();
                if (ff5_q_is_used)
                    claims++;
            }
            bool outmux_oversubscribed = claims > 1;
            if (outmux_oversubscribed) {
                DBG();
                return false;
            }
        }
    }

    // Check eight-tiles (mostly LUT-related validity)
    for (int i = 0; i < 8; i++) {
        if (lts.eights[i].dirty) {
            lts.eights[i].dirty = false;
            lts.eights[i].valid = false;

            auto lut6 = get_tags(lts.cells[(i << 4) | BEL_6LUT]);
            auto lut5 = get_tags(lts.cells[(i << 4) | BEL_5LUT]);

            // Check 6LUT
            if (lut6) {
                if (!is_slicem && (lut6->lut.is_memory || lut6->lut.is_srl)) {
                    DBG();
                    return false;
                } // Memory and SRLs only valid in SLICEMs
                if (lut6->lut.is_srl && (i >= 4)) {
                    DBG();
                    return false;
                }
                if (lut6->lut.is_memory || lut6->lut.is_srl) {
                    if (wclk == nullptr)
                        wclk = lut6->lut.wclk;
                    else if (lut6->lut.wclk != wclk) {
                        DBG();
                        return false;
                    }
                }
                if (lut5) {
                    // Can't mix memory and non-memory
                    if (lut6->lut.is_memory != lut5->lut.is_memory || lut6->lut.is_srl != lut5->lut.is_srl) {
                        DBG();
                        return false;
                    }
                    // SRL16E pairs are exempt from the input/output sharing
                    // checks below: their address pins are constant-tied and
                    // shared by construction (Vivado packs two SRL16Es per
                    // slot routinely).  (Port of nextpnr-xilinx.)
                    bool srl_pair = lut6->lut.is_srl && lut5->lut.is_srl;
                    // If all 6 inputs or 2 outputs are used, 5LUT can't also be present
                    bool lut6_uses_whole_lut = lut6->lut.input_count == 6 || lut6->lut.output_count == 2;
                    bool lut6_excludes_lut5 = !srl_pair && lut6_uses_whole_lut;
                    if (lut6_excludes_lut5) {
                        DBG();
                        return false;
                    }
                    // If more than 5 total inputs are used, need to check number of shared input
                    bool must_share_inputs = !srl_pair && (lut6->lut.input_count + lut5->lut.input_count) > 5;
                    if (must_share_inputs) {
                        int shared = 0, need_shared = (lut6->lut.input_count + lut5->lut.input_count - 5);
                        for (int j = 0; j < lut6->lut.input_count; j++) {
                            for (int k = 0; k < lut5->lut.input_count; k++) {
                                if (lut6->lut.input_sigs[j] == lut5->lut.input_sigs[k])
                                    shared++;
                                if (shared >= need_shared)
                                    break;
                            }
                        }
                        if (shared < need_shared) {
                            DBG();
                            return false;
                        }
                    }
                }
            }
            if (lut5 != nullptr) {
                if (!is_slicem && (lut5->lut.is_memory || lut5->lut.is_srl)) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                if (lut5->lut.is_srl) {
                    if (wclk == nullptr)
                        wclk = lut5->lut.wclk;
                    else if (lut5->lut.wclk != wclk) {
                        DBG();
                        return false;
                    }
                }
                // 5LUT can use at most 5 inputs and 1 output
                if (lut5->lut.input_count > 5 || lut5->lut.output_count == 2) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
            }

            // Check (over)usage ofX inputs
            NetInfo *x_net = nullptr;
            if (lut6) {
                x_net = lut6->lut.di2_net;
            }

            CellInfo *mux_cell = nullptr;
            // Eights A, C, E, G: F7MUX uses X input
            if (i == 0 || i == 2 || i == 4 || i == 6)
                mux_cell = lts.cells[i << 4 | BEL_F7MUX];
            // Eights B, F: F8MUX uses X input
            if (i == 1 || i == 5)
                mux_cell = lts.cells[(i - 1) << 4 | BEL_F8MUX];
            auto mux = get_tags(mux_cell);
            if (mux) {
                if (!x_net)
                    x_net = mux->mux.sel;
                else if (x_net != mux->mux.sel) {
                    DBG();
                    return false; // conflict between existing X use and mux select
                }
            }

            CellInfo *out_fmux_cell = nullptr;
            // Subslices A, C: F7MUX connects to F7F8 out
            if (i == 0 || i == 2 || i == 4 || i == 6)
                out_fmux_cell = lts.cells[(i << 4) | BEL_F7MUX];
            // Subslices B: F8MUX connects to F7F8 out
            if (i == 1 || i == 5)
                out_fmux_cell = lts.cells[(i - 1) << 4 | BEL_F8MUX];

            auto carry4 = get_tags(lts.cells[((i / 4) << 6) | BEL_CARRY4]);

            if (carry4 != nullptr && carry4->carry.x_sigs[i % 4] != nullptr) {
                if (x_net == nullptr)
                    x_net = carry4->carry.x_sigs[i % 4];
                else if (x_net != carry4->carry.x_sigs[i % 4]) {
                    DBG();
                    return false;
                }
            }

            // FF1 might use X, if it isn't driven directly
            CellInfo *ff1_cell = lts.cells[i << 4 | BEL_FF];
            auto ff1 = get_tags(ff1_cell);
            bool ff1_uses_x = false;
            if (ff1 != nullptr && ff1->ff.d != nullptr && ff1->ff.d->driver.cell != nullptr) {
                auto &drv = ff1->ff.d->driver;
                CellInfo *carry4_cell = lts.cells[((i / 4) << 6) | BEL_CARRY4];
                // Direct feed shapes reach the main FF without the X pin:
                // LUT O6/O5, F7/F8 mux out, or THIS position's CARRY4 O/CO via
                // the XOR/CY xFFMUX paths.  The position check matters: an FF
                // fed by another position's carry output must NOT pass as
                // "direct" here, because position A's FFMUX cannot see O3.
                bool driven_by_this_slice_carry = carry4 && drv.cell == carry4_cell;
                bool driven_by_own_position_carry =
                        driven_by_this_slice_carry &&
                        (carry4->carry.out_sigs[i % 4] == ff1->ff.d || carry4->carry.cout_sigs[i % 4] == ff1->ff.d);
                bool ff1_fed_directly = (drv.cell == lts.cells[(i << 4) | BEL_6LUT] && drv.port != id_MC31) ||
                                        drv.cell == lts.cells[(i << 4) | BEL_5LUT] || drv.cell == out_fmux_cell ||
                                        driven_by_own_position_carry;
                if (ff1_fed_directly) {
                    // Direct, OK
                } else {
                    // With the direct feeds excluded, a carry driver can only
                    // be ANOTHER position's output of this very slice's carry.
                    // There is no realizable path: this position's xFFMUX sees
                    // only its own O/CO, and the X input comes from the fabric
                    // -- whose source would be this same site, an exit-and-
                    // reenter the router does not model for intra-site arcs
                    // ("Failed to route arc ... CARRY4_O2 to AFFMUX_OUT").
                    // Flat reject; the FF is placeable in any OTHER slice via
                    // a normal fabric route.  (Port of nextpnr-xilinx.)
                    if (driven_by_this_slice_carry) {
                        DBG();
                        return false;
                    }
                    // Indirect, must use X input
                    ff1_uses_x = true;
                    if (x_net == nullptr)
                        x_net = ff1->ff.d;
                    else if (x_net != ff1->ff.d) {
                        DBG();
                        return false;
                    }
                }
            }

            // FF2 might use X, if it isn't driven directly
            auto ff2 = get_tags(lts.cells[i << 4 | BEL_FF2]);
            if (ff2 != nullptr && ff2->ff.d != nullptr && ff2->ff.d->driver.cell != nullptr) {
                auto &drv = ff2->ff.d->driver;
                CellInfo *carry4_cell = lts.cells[((i / 4) << 6) | BEL_CARRY4];
                // The 5FF's only X-free feed is its own position's O5
                bool driven_by_this_slice_carry = carry4 && drv.cell == carry4_cell;
                if (drv.cell == lts.cells[(i << 4) | BEL_5LUT]) {
                    // Direct, OK
                } else {
                    // The 5FF's D mux sees only O5 and the X bypass -- a carry
                    // output can NEVER reach it directly, at any position, and
                    // via X it would need the same exit-and-reenter the router
                    // does not model.  Flat reject (same class as the main-FF
                    // case above).  (Port of nextpnr-xilinx.)
                    if (driven_by_this_slice_carry) {
                        DBG();
                        return false;
                    }
                    // Indirect, must use X input
                    if (x_net == nullptr)
                        x_net = ff2->ff.d;
                    else if (x_net != ff2->ff.d) {
                        DBG();
                        return false;
                    }
                }
            }

            // Legalisation fix (xc7): when the main FF (BEL_FF) takes its D
            // from the X bypass (indirect) AND the column's secondary "5" FF
            // (BEL_FF2) is also placed, the router fails to bind the main FF's
            // input MUX (xFFMUX) from X -- it silently leaves the main FF's D
            // floating (observed via physical sim: the bypass-fed AFF's D = X,
            // corrupting an LFSR).  Forbid that co-pack so the placer
            // separates the two FFs.  (Port of nextpnr-xilinx.)
            bool bypass_fed_ff_shares_with_5ff = ff1_uses_x && ff2 != nullptr;
            if (bypass_fed_ff_shares_with_5ff) {
                DBG();
                return false;
            }

            // collision with top address bits
            if (tile_is_memory && !small_memory) {
                auto top_lut = get_tags(lts.cells[(3 << 4) | BEL_6LUT]);
                if (top_lut) {
                    if ((i == 2) && x_net != top_lut->lut.address_msb[0]) {
                        DBG();
                        return false;
                    }
                    if ((i == 1) && x_net != top_lut->lut.address_msb[1]) {
                        DBG();
                        return false;
                    }
                }
            }

            bool mux_output_used = false;
            NetInfo *out5 = nullptr;
            if (lut6 != nullptr && lut6->lut.output_count == 2)
                out5 = lut6->lut.output_sigs[1];
            else if (lut5 != nullptr && !lut5->lut.only_drives_carry)
                out5 = lut5->lut.output_sigs[0];
            if (out5 != nullptr && (out5->users.entries() > 1 ||
                                    ((ff1 == nullptr || out5 != ff1->ff.d) && (ff2 == nullptr || out5 != ff2->ff.d)))) {
                mux_output_used = true;
            }

            if (carry4 != nullptr && carry4->carry.out_sigs[i % 4] != nullptr) {
                NetInfo *o = carry4->carry.out_sigs[i % 4];
                if (o->users.entries() > 1 || (ff1 == nullptr || o != ff1->ff.d)) {
                    if (mux_output_used) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                    mux_output_used = true;
                }
            }
            // A carry CO with fabric (or other) consumers also claims the
            // subslice's single output mux.  This check stops the placer from
            // co-locating a CARRY4 CO and a 5FF into an unroutable slot.
            // CO3 leaves on the dedicated COUT->CIN spine, so only its
            // non-CIN users count; CO0..CO2 can only reach the fabric
            // through the output mux at all.  (Port of nextpnr-xilinx.)
            bool carry_co_is_used = carry4 != nullptr && carry4->carry.cout_sigs[i % 4] != nullptr;
            if (carry_co_is_used) {
                NetInfo *co = carry4->carry.cout_sigs[i % 4];
                bool co_uses_mux = false;
                bool co_is_chain_output = (i % 4) == 3;
                if (co_is_chain_output) {
                    for (auto &usr : co->users)
                        if (usr.port != id_CIN)
                            co_uses_mux = true;
                } else {
                    co_uses_mux = !co->users.empty();
                }
                if (co_uses_mux) {
                    if (mux_output_used) {
                        DBG();
                        return false;
                    }
                    mux_output_used = true;
                }
            }
            if (out_fmux_cell != nullptr) {
                auto out_fmux = get_tags(out_fmux_cell);
                NetInfo *f7f8 = out_fmux->mux.out;
                if (f7f8 != nullptr && (f7f8->users.entries() > 1 || ((ff1 == nullptr || f7f8 != ff1->ff.d)))) {
                    if (mux_output_used) {
                        DBG();
                        return false; // Memory and SRLs only valid in SLICEMs
                    }
                    mux_output_used = true;
                }
            }
            if (ff2 != nullptr) {
                if (mux_output_used) {
                    DBG();
                    return false; // Memory and SRLs only valid in SLICEMs
                }
                mux_output_used = true;
            }

            lts.eights[i].valid = true;
        } else if (!lts.eights[i].valid) {
            DBG();
            return false;
        }
    }
    // Check half-tiles
    for (int i = 0; i < 2; i++) {
        if (lts.halfs[i].dirty) {
            lts.halfs[i].valid = false;
            bool found_ff[2] = {false, false};
            if (i == 0 && wclk == nullptr) {
                // Need to check wclk too
                for (int z = 4 * i; z < 4 * (i + 1); z++) {
                    for (int k = 0; k < 2; k++) {
                        auto lut = get_tags(lts.cells[z << 4 | (BEL_6LUT + k)]);
                        if (!lut)
                            continue;
                        if (!lut->lut.is_memory && !lut->lut.is_srl)
                            continue;
                        if (lut->lut.wclk != nullptr) {
                            wclk = lut->lut.wclk;
                            break;
                        }
                    }
                }
            }
            NetInfo *clk = nullptr, *sr = nullptr, *ce = nullptr;
            bool clkinv = false, srinv = false, islatch = false, ffsync = false;
            for (int z = 4 * i; z < 4 * (i + 1); z++) {
                for (int k = 0; k < 2; k++) {
                    auto ff = get_tags(lts.cells[z << 4 | (BEL_FF + k)]);
                    if (ff == nullptr)
                        continue;
                    if (ff->ff.is_latch && k == 1) {
                        DBG();
                        return false;
                    }
                    if (found_ff[0] || found_ff[1]) {
                        if (ff->ff.clk != clk) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.sr != sr) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.ce != ce) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.is_clkinv != clkinv) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.is_srinv != srinv) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.is_latch != islatch) {
                            DBG();
                            return false;
                        }
                        if (ff->ff.ffsync != ffsync) {
                            DBG();
                            return false;
                        }
                    } else {
                        clk = ff->ff.clk;
                        if (i == 0 && wclk != nullptr && clk != wclk) {
                            DBG();
                            return false;
                        }
                        sr = ff->ff.sr;
                        ce = ff->ff.ce;
                        clkinv = ff->ff.is_clkinv;
                        srinv = ff->ff.is_srinv;
                        islatch = ff->ff.is_latch;
                        ffsync = ff->ff.ffsync;
                    }
                    found_ff[k] = true;
                }
            }
            lts.halfs[i].valid = true;
        } else if (!lts.halfs[i].valid) {
            DBG();
            return false;
        }
    }
    return true;
}

bool XilinxImpl::isBelLocationValid(BelId bel, bool explain_invalid) const
{
    if (!frozen_tiles.empty() && frozen_tiles.count(bel.tile)) {
        auto unpinned = [&](CellInfo *ci) { return ci != nullptr && !ci->attrs.count(id_BEL); };
        if (is_logic_tile(bel) && tile_status.at(bel.tile).lts) {
            for (CellInfo *ci : tile_status.at(bel.tile).lts->cells)
                if (unpinned(ci))
                    return false;
        } else if (is_bram_tile(bel) && tile_status.at(bel.tile).bts) {
            for (CellInfo *ci : tile_status.at(bel.tile).bts->cells)
                if (unpinned(ci))
                    return false;
        }
    }
    if (is_logic_tile(bel)) {
        if (!tile_status.at(bel.tile).lts)
            return true;
        return xc7_logic_tile_valid(bel_tile_type(bel), *tile_status.at(bel.tile).lts);
    } else if (is_bram_tile(bel)) {
        const auto &bts = tile_status.at(bel.tile).bts;
        if (!bts)
            return true;
        auto onehot = [&](CellInfo *a, CellInfo *b, CellInfo *c) {
            return (((a != nullptr) ? 1 : 0) + ((b != nullptr) ? 1 : 0) + ((c != nullptr) ? 1 : 0)) <= 1;
        };
        // Only one type of BRAM cell at any given location
        if (!onehot(bts->cells[BEL_RAMFIFO36], bts->cells[BEL_RAM36], bts->cells[BEL_FIFO36])) {
            DBG();
            return false;
        }
        if (!onehot(bts->cells[BEL_RAMFIFO18_L], bts->cells[BEL_RAM18_L], bts->cells[BEL_FIFO18_L])) {
            DBG();
            return false;
        }
        // 18-bit BRAMs cannot be used whilst 36-bit is used
        if (bts->cells[BEL_RAMFIFO36] || bts->cells[BEL_RAM36] || bts->cells[BEL_FIFO36]) {
            for (int i = 4; i < 12; i++)
                if (bts->cells[i]) {
                    DBG();
                    return false;
                }
        }
    }
    return true;
}

void XilinxImpl::fixup_placement()
{
    log_info("Running post-placement legalisation...\n");
    for (auto &ts : tile_status) {
        if (!ts.lts)
            continue;
        auto &lt = *(ts.lts);
        for (int z = 0; z < 8; z++) {
            // Fixup LUT connectivity - applies whenever a LUT5 is used
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            if (!lut5)
                continue;
            auto l5_tags = get_tags(lut5);
            dict<IdString, std::vector<int>> lut5Inputs, lut6Inputs;
            for (int i = 0; i < l5_tags->lut.input_count; i++)
                if (l5_tags->lut.input_sigs[i])
                    lut5Inputs[l5_tags->lut.input_sigs[i]->name].push_back(i);
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut6) {
                auto l6_tags = get_tags(lut6);
                for (int i = 0; i < l6_tags->lut.input_count; i++)
                    if (l6_tags->lut.input_sigs[i])
                        lut6Inputs[l6_tags->lut.input_sigs[i]->name].push_back(i);
            }
            if (l5_tags->lut.is_memory || l5_tags->lut.is_srl) {
                if (lut6) {
                    if (!lut6->ports.count(id_A6)) {
                        lut6->addInput(id_A6);
                    }
                    lut6->connectPort(id_A6, ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
                }
                continue;
            }
            std::set<IdString> uniqueInputs;
            for (auto i5 : lut5Inputs)
                uniqueInputs.insert(i5.first);
            for (auto i6 : lut6Inputs)
                uniqueInputs.insert(i6.first);
            // Disconnect LUT inputs, and re-connect them to not overlap
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto p : ports) {
                lut5->disconnectPort(p);
                lut5->attrs.erase(ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx)));
                if (lut6) {
                    lut6->attrs.erase(ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx)));
                    lut6->disconnectPort(p);
                }
            }
            int index = 0;
            for (auto i : uniqueInputs) {
                if (lut5Inputs.count(i)) {
                    if (!lut5->ports.count(ports[index])) {
                        lut5->ports[ports[index]].name = ports[index];
                        lut5->ports[ports[index]].type = PORT_IN;
                    }
                    lut5->connectPort(ports[index], ctx->nets.at(i).get());
                    lut5->attrs[ctx->idf("X_ORIG_PORT_%s", ports[index].c_str(ctx))] = std::string("");
                    bool first = true;
                    for (auto inp : lut5Inputs[i]) {
                        lut5->attrs[ctx->idf("X_ORIG_PORT_%s", ports[index].c_str(ctx))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                if (lut6 && lut6Inputs.count(i)) {
                    if (!lut6->ports.count(ports[index])) {
                        lut6->ports[ports[index]].name = ports[index];
                        lut6->ports[ports[index]].type = PORT_IN;
                    }
                    lut6->connectPort(ports[index], ctx->nets.at(i).get());

                    lut6->attrs[ctx->idf("X_ORIG_PORT_%s", ports[index].c_str(ctx))] = std::string("");
                    bool first = true;
                    for (auto inp : lut6Inputs[i]) {
                        lut6->attrs[ctx->idf("X_ORIG_PORT_%s", ports[index].c_str(ctx))].str +=
                                (first ? "I" : " I") + std::to_string(inp);
                        first = false;
                    }
                }
                ++index;
            }
            lut5->renamePort(id_O6, id_O5);
            lut5->attrs.erase(id_X_ORIG_PORT_O6);
            lut5->attrs[id_X_ORIG_PORT_O5] = std::string("O");

            if (lut6) {
                if (!lut6->ports.count(id_A6)) {
                    lut6->addInput(id_A6);
                }
                lut6->connectPort(id_A6, ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
            }
        }
    }
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_PS7_PS7) {
            log_info("Tieing unused PS7 inputs to constants...\n");
            for (IdString pname : ctx->getBelPins(ci->bel)) {
                if (ci->ports.count(pname) && ci->ports.at(pname).net != nullptr &&
                    ci->ports.at(pname).net->driver.cell != nullptr)
                    continue;
                if (ctx->getBelPinType(ci->bel, pname) != PORT_IN)
                    continue;
                std::string name = pname.str(ctx);
                if (name.find("_PAD_") != std::string::npos)
                    continue;
                if (boost::starts_with(name, "TEST") || boost::starts_with(name, "DEBUGSELECT") ||
                    boost::starts_with(name, "MIO") || boost::starts_with(name, "DDR"))
                    continue;
                bool constval = false;
                ci->ports[pname].name = pname;
                ci->ports[pname].type = PORT_IN;
                if (ci->ports[pname].net != nullptr) {
                    ci->disconnectPort(pname);
                    ci->attrs.erase(ctx->idf("X_ORIG_PORT_", name.c_str()));
                }
                ci->connectPort(pname,
                                ctx->nets.at(constval ? ctx->id("$PACKER_VCC_NET") : ctx->id("$PACKER_GND_NET")).get());
            }
        }
    }
}

void XilinxImpl::fixup_routing()
{
    log_info("Running post-routing legalisation...\n");
    /*
     * Convert LUT permutation into correct physical connections (i.e. effectively eliminating the permutation pips),
     * then specifying the permutation as a new physical-to-logical mapping using X_ORIG_PORT. This keeps RapidWright
     * and Vivado happy, preserving the original logical netlist
     */
    dict<int, std::vector<int>> used_perm_pips; // tile -> [extra_data] for LUT perm pips

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        for (auto &wire : ni->wires) {
            PipId pip = wire.second.pip;
            if (pip == PipId())
                continue;
            auto &pd = chip_pip_info(ctx->chip_info, pip);
            if (pd.flags != PIP_LUT_PERMUTATION)
                continue;
            const auto &extra_data = *reinterpret_cast<const XlnxPipExtraDataPOD *>(pd.extra_data.get());
            used_perm_pips[pip.tile].push_back(extra_data.pip_config);
        }
    }

    for (size_t ti = 0; ti < tile_status.size(); ti++) {
        if (!used_perm_pips.count(int(ti)))
            continue;
        auto &ts = tile_status.at(ti);
        if (!ts.lts)
            continue;

        auto &lt = *ts.lts;
        for (int z = 0; z < 8; z++) {
            CellInfo *lut5 = lt.cells[z << 4 | BEL_5LUT];
            CellInfo *lut6 = lt.cells[z << 4 | BEL_6LUT];
            if (lut5 == nullptr && lut6 == nullptr)
                continue;
            auto &pp = used_perm_pips.at(ti);
            // from -> to
            dict<IdString, std::vector<IdString>> new_connections;
            IdString ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
            for (auto pip : pp) {
                if (((pip >> 8) & 0xF) != z)
                    continue;
                new_connections[ports[(pip >> 4) & 0xF]].push_back(ports[pip & 0xF]);
            }
            dict<IdString, NetInfo *> orig_nets;
            dict<IdString, std::string> orig_ports_l6, orig_ports_l5;
            for (int i = 0; i < 6; i++) {
                NetInfo *l6net = lut6 ? lut6->getPort(ports[i]) : nullptr;
                NetInfo *l5net = lut5 ? lut5->getPort(ports[i]) : nullptr;
                orig_nets[ports[i]] = (l6net ? l6net : l5net);
                if (lut6)
                    orig_ports_l6[ports[i]] =
                            str_or_default(lut6->attrs, ctx->idf("X_ORIG_PORT_%s", ports[i].c_str(ctx)));
                if (lut5)
                    orig_ports_l5[ports[i]] =
                            str_or_default(lut5->attrs, ctx->idf("X_ORIG_PORT_%s", ports[i].c_str(ctx)));
            }
            for (auto &nc : new_connections) {
                if (lut6)
                    lut6->disconnectPort(nc.first);
                if (lut5)
                    lut5->disconnectPort(nc.first);
                for (auto &dst : nc.second) {
                    if (lut6)
                        lut6->disconnectPort(dst);
                    if (lut5)
                        lut5->disconnectPort(dst);
                }
            }
            for (int i = 0; i < 6; i++) {
                if (lut6)
                    lut6->attrs.erase(ctx->idf("X_ORIG_PORT_%s", ports[i].c_str(ctx)));
                if (lut5)
                    lut5->attrs.erase(ctx->idf("X_ORIG_PORT_%s", ports[i].c_str(ctx)));
            }
            for (int i = 0; i < 6; i++) {
                auto p = ports[i];
                if (!new_connections.count(p) || new_connections.at(p).empty())
                    continue;
                if (lut6) {
                    if (!lut6->ports.count(p)) {
                        lut6->addInput(p);
                    }
                    lut6->connectPort(p, orig_nets[new_connections.at(p).front()]);
                    lut6->attrs[ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx))] = std::string("");
                    auto &orig_attr = lut6->attrs[ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l6[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut6->attrs.erase(ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx)));
                }
                if (lut5) {
                    if (!lut5->ports.count(p)) {
                        lut5->addInput(p);
                    }
                    lut5->connectPort(p, orig_nets[new_connections.at(p).front()]);
                    lut5->attrs[ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx))] = std::string("");
                    auto &orig_attr = lut5->attrs[ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx))].str;
                    bool first = true;
                    for (auto &nc : new_connections.at(p)) {
                        orig_attr += orig_ports_l5[nc] + (first ? "" : " ");
                        first = false;
                    }
                    if (orig_attr.empty())
                        lut5->attrs.erase(ctx->idf("X_ORIG_PORT_%s", p.c_str(ctx)));
                }
            }
        }
    }

    /*
     * Legalise route through OSERDESE3s
     */
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_OSERDESE3) {
            if (ci->getPort(id_T_OUT) != nullptr)
                continue;
            ci->params[id_OSERDES_T_BYPASS] = std::string("TRUE");
        }
    }
}

NEXTPNR_NAMESPACE_END
