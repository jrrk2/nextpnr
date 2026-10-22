/*
 *  nextpnr -- Next Generation Place and Route
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

// The global clock tree's insertion delay.
//
// A BUFG drives the clock spine, which runs up and down the die from its
// centre; each clock region's HROW taps the spine and drives the region's
// HCLK rows left and right, which feed the leaves.  prjxray gives the spine
// and HCLK nodes no resistance, so the routed delay of a global clock is the
// same at every sink (762 ps here) and nextpnr sees no skew at all.  The
// silicon has plenty: on an xc7vx485t, Vivado's clock delays grow by about
// 170 ps for each clock region row away from the BUFG's rows and 1.7 ps for
// each tile column away from the spine, and a block RAM's clock pin is 430 ps
// later than a flip-flop's.  On a design whose ROM block RAMs ended up at the
// top and bottom of the die that was the 0.4-0.9 ns which nextpnr reported as
// met and the board did not.
//
// The model adds that delay to each sink of a BUFGCTRL-driven net, routed or
// not, and turns on clock-skew analysis for every timing analyser, so the
// placer and router see it as well as the final report.  The region rows and
// the spine come from the chipdb's CLK_HROW and CLK_BUFG tiles, so it holds
// for any 7-series die; the rates are the xc7vx485t's.  "-o clock-tree=off"
// returns to the flat delay.

#include "log.h"
#include "nextpnr.h"
#include "xilinx.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// Vivado's clock delays on an xc7vx485t-2 (destination clock delay of
// flip-flops and block RAMs in every clock region, from a placed design)
const delay_t ct_ps_per_region_row = 170;
const delay_t ct_ps_per_10_columns = 17;
const delay_t ct_bram_clock_pin_ps = 430;
// The routed delay of every global clock sink, for unrouted estimates
const delay_t ct_flat_route_ps = 762;
} // namespace

void XilinxImpl::init_clock_tree()
{
    std::string mode =
            ctx->args.options.count("clock-tree") ? ctx->args.options.at("clock-tree").as<std::string>() : "on";
    if (mode == "off")
        return;
    std::vector<int> bufg_rows;
    for (int tile = 0; tile < ctx->chip_info->tile_insts.ssize(); tile++) {
        const std::string prefix = IdString(tile_extra_data(tile)->name_prefix).str(ctx);
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        const bool is_hrow = prefix.rfind("CLK_HROW_", 0) == 0;
        const bool is_bufg = prefix.rfind("CLK_BUFG_TOP", 0) == 0 || prefix.rfind("CLK_BUFG_BOT", 0) == 0;
        if (is_hrow) {
            ct_region_rows.push_back(y);
            ct_spine_x = x;
        }
        if (is_bufg)
            bufg_rows.push_back(y);
    }
    std::sort(ct_region_rows.begin(), ct_region_rows.end());
    ct_region_rows.erase(std::unique(ct_region_rows.begin(), ct_region_rows.end()), ct_region_rows.end());
    const bool device_has_a_clock_tree = !ct_region_rows.empty() && !bufg_rows.empty() && ct_spine_x >= 0;
    if (!device_has_a_clock_tree) {
        log_info("clock-tree: no CLK_HROW/CLK_BUFG tiles, global clocks keep their routed delay\n");
        return;
    }
    int sum = 0;
    for (int y : bufg_rows)
        sum += y;
    ct_centre_y = float(sum) / bufg_rows.size();
    for (auto bel : ctx->getBels()) {
        const std::string type = ctx->getBelType(bel).str(ctx);
        if (type.rfind("BUFGCTRL", 0) == 0)
            ct_bufg_bel_types.insert(ctx->getBelType(bel));
    }
    ct_enabled = true;
    ctx->settings[ctx->id("timing/clockSkew")] = Property(1, 1);
    log_info("clock-tree: %d clock region rows, spine at x=%d, BUFGs at y=%.1f\n", int(ct_region_rows.size()),
             ct_spine_x, ct_centre_y);
}

bool XilinxImpl::is_global_clock_bel(BelId bel) const
{
    return bel != BelId() && ct_bufg_bel_types.count(ctx->getBelType(bel));
}

delay_t XilinxImpl::clock_tree_delay_at(int x, int y, bool bram_clock_pin) const
{
    if (!ct_enabled)
        return 0;
    // the sink's region is the one whose HROW row is nearest; its distance
    // from the BUFGs is the number of region rows between them
    int region_row = ct_region_rows.front();
    for (int r : ct_region_rows)
        if (std::abs(r - y) < std::abs(region_row - y))
            region_row = r;
    const float region_offset = region_row - ct_centre_y;
    int rows_away = 0;
    for (int r : ct_region_rows) {
        const float offset = r - ct_centre_y;
        const bool same_side = (offset > 0) == (region_offset > 0);
        const bool between = same_side && std::abs(offset) < std::abs(region_offset);
        if (between)
            rows_away++;
    }
    delay_t delay = ct_ps_per_region_row * rows_away + ct_ps_per_10_columns * std::abs(x - ct_spine_x) / 10;
    if (bram_clock_pin)
        delay += ct_bram_clock_pin_ps;
    return delay;
}

delay_t XilinxImpl::clock_tree_delay(BelId sink) const
{
    if (!ct_enabled || sink == BelId())
        return 0;
    int x, y;
    tile_xy(ctx->chip_info, sink.tile, x, y);
    return clock_tree_delay_at(x, y, is_bram_tile(sink));
}

bool XilinxImpl::getArcDelayOverride(const NetInfo *net_info, const PortRef &sink, DelayQuad &delay) const
{
    if (!ct_enabled)
        return false;
    const CellInfo *driver = net_info->driver.cell;
    const bool driven_by_a_bufg = driver != nullptr && is_global_clock_bel(driver->bel);
    if (!driven_by_a_bufg || sink.cell == nullptr)
        return false;
    // The routed delay, summed as Context::getNetinfoRouteDelayQuad does
    WireId src_wire = ctx->getNetinfoSourceWire(net_info);
    if (src_wire == WireId())
        return false;
    bool found = false;
    DelayQuad result(std::numeric_limits<delay_t>::max(), std::numeric_limits<delay_t>::lowest());
    for (auto dst_wire : ctx->getNetinfoSinkWires(net_info, sink)) {
        WireId cursor = dst_wire;
        DelayQuad d{0};
        while (cursor != WireId() && cursor != src_wire) {
            auto it = net_info->wires.find(cursor);
            if (it == net_info->wires.end() || it->second.pip == PipId())
                break;
            d = d + ctx->getPipDelay(it->second.pip) + ctx->getWireDelay(cursor);
            cursor = ctx->getPipSrcWire(it->second.pip);
        }
        const bool arc_is_routed = cursor == src_wire;
        if (!arc_is_routed)
            return false;
        d = d + ctx->getWireDelay(src_wire);
        result = DelayQuad(std::min(result.minDelay(), d.minDelay()), std::max(result.maxDelay(), d.maxDelay()));
        found = true;
    }
    if (!found)
        return false;
    delay = result + DelayQuad(clock_tree_delay(sink.cell->bel));
    return true;
}

delay_t XilinxImpl::predict_clock_tree_delay(BelId src_bel, BelId dst_bel) const
{
    if (!ct_enabled || !is_global_clock_bel(src_bel))
        return -1;
    return ct_flat_route_ps + clock_tree_delay(dst_bel);
}

NEXTPNR_NAMESPACE_END
