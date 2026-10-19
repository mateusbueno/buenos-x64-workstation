#!/usr/bin/env python3
"""Parametric unit-economics model for the Ladiani Velora 73 (Story 11.4 prep).

DRAFT FOR FOUNDER. This is ARITHMETIC, not tax advice. It contains NO tax rates,
exchange rates or prices of its own: every monetary and tax-rate input comes from
an inputs JSON file that the founder and the tax advisor fill in
(see margin_inputs.template.json). It does not satisfy Story 11.4 acceptance
criteria; the advisor's written report does.

Conventions
-----------
* All rates are PERCENT numbers (10 means 10 %), never fractions.
* Money is BRL unless a field name says _usd.
* The import-tax cascade below is a deliberately simple, STANDARD-SHAPE
  placeholder (II on customs value; IPI on customs value + II; PIS/COFINS-import
  on customs value; ICMS "por dentro" on everything before it). Whether that
  shape, the tax bases and the credit fractions are right for this company is
  exactly what the tax advisor must confirm. Change the code if the advisor's
  structure differs.
* Revenue taxes are a generic list of {name, rate_pct} applied to the revenue the
  company receives, so any regime (Simples, Lucro Presumido, ...) can be entered.
* Margin % is expressed over the customer-facing list price.

Usage
-----
  python3 margin_model.py --inputs my_inputs.json [--json] [--target-margin-pct N]
  python3 margin_model.py --self-test
"""
import argparse
import json
import sys

DEFAULT_PRICES = {"v1": [17990, 18990, 19990], "pro": [24990]}  # from go-to-market.md / Story 11.4
SCENARIOS = ("direct_pix_boleto", "direct_card_10x", "dealer")


# --------------------------------------------------------------------------- validation
def _missing(v):
    return v is None or (isinstance(v, str) and v.strip().upper() in ("", "TODO"))


def _need(d, key, path, errs):
    if not isinstance(d, dict) or key not in d or _missing(d[key]):
        errs.append(f"{path}.{key}")
        return None
    return d[key]


def validate(inp):
    errs = []
    _need(inp, "fx_brl_per_usd", "", errs)
    _need(inp, "units", "", errs)
    lg = inp.get("import_logistics") or {}
    for k in ("international_freight_insurance_brl", "customs_brokerage_brl"):
        _need(lg, k, "import_logistics", errs)
    cr = inp.get("credits") or {}
    for k in ("ipi_credit_pct", "icms_credit_pct", "pis_cofins_credit_pct"):
        _need(cr, k, "credits", errs)
    oc = inp.get("other_unit_costs_brl") or {}
    for k in ("assembly", "packaging", "outbound_freight"):
        _need(oc, k, "other_unit_costs_brl", errs)
    rt = inp.get("revenue_taxes_pct")
    if not isinstance(rt, list) or not rt:
        errs.append("revenue_taxes_pct (non-empty list of {name, rate_pct})")
    else:
        for i, t in enumerate(rt):
            _need(t, "name", f"revenue_taxes_pct[{i}]", errs)
            _need(t, "rate_pct", f"revenue_taxes_pct[{i}]", errs)
    _need(inp, "warranty_reserve_pct_of_revenue", "", errs)
    ch = inp.get("channel") or {}
    for k in ("pix_boleto_fee_pct", "pix_boleto_fee_fixed_brl", "card_mdr_pct",
              "card_anticipation_pct", "card_fee_fixed_brl", "dealer_margin_pct",
              "dealer_channel_fee_pct"):
        _need(ch, k, "channel", errs)
    tiers = inp.get("tiers") or {}
    if not tiers:
        errs.append("tiers")
    for tname, t in tiers.items():
        if t is None:
            continue
        lines = t.get("bom_lines")
        if not isinstance(lines, list) or not lines:
            errs.append(f"tiers.{tname}.bom_lines")
            continue
        for i, ln in enumerate(lines):
            p = f"tiers.{tname}.bom_lines[{i}]"
            _need(ln, "name", p, errs)
            origin = _need(ln, "origin", p, errs)
            usd, brl = ln.get("cost_usd"), ln.get("cost_brl")
            if _missing(usd) == _missing(brl):
                errs.append(f"{p}.cost_usd XOR cost_brl (exactly one)")
            if origin == "imported":
                for k in ("ii_rate_pct", "ipi_rate_pct", "pis_import_rate_pct",
                          "cofins_import_rate_pct", "icms_import_rate_pct",
                          "ex_tarifario_applies"):
                    _need(ln, k, p, errs)
                if ln.get("ex_tarifario_applies") is True:
                    _need(ln, "ii_rate_pct_ex_tarifario", p, errs)
            elif origin not in (None, "local") and not _missing(origin):
                errs.append(f"{p}.origin must be 'imported' or 'local'")
        pp = t.get("price_points_brl")
        if pp is not None and not (isinstance(pp, list) and pp):
            errs.append(f"tiers.{tname}.price_points_brl (null for default, or list)")
    return errs


# --------------------------------------------------------------------------- core arithmetic
def line_goods_brl(ln, fx):
    return ln["cost_usd"] * fx if not _missing(ln.get("cost_usd")) else ln["cost_brl"]


def landed_cost(inp, tier):
    """Per-unit landed + other direct cost for one tier. Returns dict of components."""
    fx = inp["fx_brl_per_usd"]
    lg, cr, oc = inp["import_logistics"], inp["credits"], inp["other_unit_costs_brl"]
    lines = tier["bom_lines"]
    imported = [l for l in lines if l["origin"] == "imported"]
    local = [l for l in lines if l["origin"] == "local"]
    goods_imp = sum(line_goods_brl(l, fx) for l in imported)
    local_cost = sum(line_goods_brl(l, fx) for l in local)
    freight = lg["international_freight_insurance_brl"] if imported else 0.0
    t_ii = t_ipi = t_pc = t_icms = cv_total = 0.0
    detail = []
    for l in imported:
        g = line_goods_brl(l, fx)
        cv = g + (freight * g / goods_imp if goods_imp else 0.0)
        ii_rate = l["ii_rate_pct_ex_tarifario"] if l["ex_tarifario_applies"] else l["ii_rate_pct"]
        ii = cv * ii_rate / 100
        ipi = (cv + ii) * l["ipi_rate_pct"] / 100
        pis = cv * l["pis_import_rate_pct"] / 100
        cofins = cv * l["cofins_import_rate_pct"] / 100
        icms_rate = l["icms_import_rate_pct"] / 100
        base = (cv + ii + ipi + pis + cofins) / (1 - icms_rate)
        icms = base * icms_rate
        cv_total += cv
        t_ii += ii
        t_ipi += ipi
        t_pc += pis + cofins
        t_icms += icms
        detail.append({"name": l["name"], "customs_value": cv, "ii": ii, "ipi": ipi,
                       "pis_cofins": pis + cofins, "icms": icms})
    credits = (t_ipi * cr["ipi_credit_pct"] + t_icms * cr["icms_credit_pct"]
               + t_pc * cr["pis_cofins_credit_pct"]) / 100
    import_taxes_net = t_ii + t_ipi + t_pc + t_icms - credits
    brokerage = lg["customs_brokerage_brl"] if imported else 0.0
    other = oc["assembly"] + oc["packaging"] + oc["outbound_freight"]
    total = local_cost + goods_imp + freight + import_taxes_net + brokerage + other
    return {"local_goods": local_cost, "imported_goods": goods_imp, "intl_freight_insurance": freight,
            "ii": t_ii, "ipi": t_ipi, "pis_cofins": t_pc, "icms": t_icms,
            "credits_recovered": credits, "import_taxes_net": import_taxes_net,
            "customs_brokerage": brokerage, "other_direct": other, "total_cost": total,
            "import_detail": detail}


def scenario_margin(inp, cost_total, price, scenario):
    """Return dict of revenue, deductions and margin for one price and scenario."""
    ch = inp["channel"]
    tax_pct = sum(t["rate_pct"] for t in inp["revenue_taxes_pct"])
    w_pct = inp["warranty_reserve_pct_of_revenue"]
    if scenario == "direct_pix_boleto":
        d, f_pct, f_fix = 0.0, ch["pix_boleto_fee_pct"], ch["pix_boleto_fee_fixed_brl"]
    elif scenario == "direct_card_10x":
        d = 0.0
        f_pct = ch["card_mdr_pct"] + ch["card_anticipation_pct"]
        f_fix = ch["card_fee_fixed_brl"]
    elif scenario == "dealer":
        d, f_pct, f_fix = ch["dealer_margin_pct"], ch["dealer_channel_fee_pct"], 0.0
    else:
        raise ValueError(scenario)
    revenue = price * (1 - d / 100)
    taxes = revenue * tax_pct / 100
    warranty = revenue * w_pct / 100
    fees = price * f_pct / 100 + f_fix
    margin = revenue - taxes - warranty - fees - cost_total
    # margin = price*k - c  (linear in price) -> used for the minimum viable price
    k = (1 - d / 100) * (1 - tax_pct / 100 - w_pct / 100) - f_pct / 100
    c = cost_total + f_fix
    return {"price": price, "revenue_to_company": revenue, "revenue_taxes": taxes,
            "warranty_reserve": warranty, "channel_fees": fees, "cost": cost_total,
            "margin_brl": margin, "margin_pct_of_list": margin / price * 100, "_k": k, "_c": c}


def min_price(k, c, target_pct):
    """Smallest list price with margin >= target_pct of list price; None if impossible."""
    denom = k - target_pct / 100
    return c / denom if denom > 0 else None


def run(inp, target_pct=0.0):
    out = {}
    for tname, tier in inp["tiers"].items():
        if tier is None:
            continue
        cost = landed_cost(inp, tier)
        prices = tier.get("price_points_brl") or DEFAULT_PRICES.get(tname) or []
        rows = {}
        for sc in SCENARIOS:
            rows[sc] = [scenario_margin(inp, cost["total_cost"], p, sc) for p in prices]
            k, c = rows[sc][0]["_k"], rows[sc][0]["_c"]
            rows[sc + "__min_price_for_target"] = min_price(k, c, target_pct)
        out[tname] = {"cost": cost, "scenarios": rows, "target_margin_pct": target_pct}
    return out


def render(res):
    lines = ["DRAFT arithmetic only, not tax advice. Inputs are the founder's/advisor's, not the model's.", ""]
    for tname, r in res.items():
        c = r["cost"]
        lines.append(f"== Tier {tname} ==")
        lines.append(f"  landed+direct cost per unit: {c['total_cost']:,.2f} BRL")
        for k in ("local_goods", "imported_goods", "intl_freight_insurance", "ii", "ipi", "pis_cofins",
                  "icms", "credits_recovered", "import_taxes_net", "customs_brokerage", "other_direct"):
            lines.append(f"    {k:24s} {c[k]:>12,.2f}")
        for sc in SCENARIOS:
            lines.append(f"  -- {sc} --")
            for row in r["scenarios"][sc]:
                lines.append(f"    price {row['price']:>9,.0f}  revenue {row['revenue_to_company']:>10,.2f}"
                             f"  taxes {row['revenue_taxes']:>9,.2f}  warranty {row['warranty_reserve']:>8,.2f}"
                             f"  fees {row['channel_fees']:>8,.2f}  margin {row['margin_brl']:>10,.2f}"
                             f"  ({row['margin_pct_of_list']:.1f}% of list)")
            mp = r["scenarios"][sc + "__min_price_for_target"]
            lines.append(f"    minimum list price for {r['target_margin_pct']:g}% margin: "
                         + (f"{mp:,.2f}" if mp is not None else "not reachable at any price"))
        lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------- self-test
def self_test():
    """Obviously fake round numbers; they exist ONLY inside this test."""
    def close(a, b, eps=1e-6):
        assert abs(a - b) < eps, (a, b)

    fake = {
        "fx_brl_per_usd": 2.0, "units": 10,
        "import_logistics": {"international_freight_insurance_brl": 0, "customs_brokerage_brl": 0},
        "credits": {"ipi_credit_pct": 0, "icms_credit_pct": 0, "pis_cofins_credit_pct": 0},
        "other_unit_costs_brl": {"assembly": 100, "packaging": 0, "outbound_freight": 0},
        "revenue_taxes_pct": [{"name": "fake tax", "rate_pct": 10}],
        "warranty_reserve_pct_of_revenue": 5,
        "channel": {"pix_boleto_fee_pct": 0, "pix_boleto_fee_fixed_brl": 0, "card_mdr_pct": 6,
                    "card_anticipation_pct": 4, "card_fee_fixed_brl": 0, "dealer_margin_pct": 30,
                    "dealer_channel_fee_pct": 0},
        "tiers": {"t": {"price_points_brl": [1000], "bom_lines": [
            {"name": "imp", "origin": "imported", "cost_usd": 100, "cost_brl": None, "ii_rate_pct": 10,
             "ex_tarifario_applies": False, "ii_rate_pct_ex_tarifario": None, "ipi_rate_pct": 0,
             "pis_import_rate_pct": 0, "cofins_import_rate_pct": 0, "icms_import_rate_pct": 0},
            {"name": "loc", "origin": "local", "cost_usd": None, "cost_brl": 80}]}},
    }
    assert validate(fake) == []
    res = run(fake, target_pct=30)["t"]
    close(res["cost"]["total_cost"], 400)           # 200 goods + 20 II + 80 local + 100 assembly
    s = res["scenarios"]
    close(s["direct_pix_boleto"][0]["margin_brl"], 450)   # 1000-100-50-400
    close(s["direct_card_10x"][0]["margin_brl"], 350)     # minus 10 % fee
    close(s["dealer"][0]["margin_brl"], 195)              # 700-70-35-400
    close(s["direct_pix_boleto__min_price_for_target"], 400 / (0.85 - 0.30))
    assert s["dealer__min_price_for_target"] is not None
    # ICMS "por dentro" and credits
    f2 = json.loads(json.dumps(fake))
    f2["tiers"]["t"]["bom_lines"][0].update(ii_rate_pct=0, icms_import_rate_pct=20)
    f2["credits"]["icms_credit_pct"] = 100
    c2 = landed_cost(f2, f2["tiers"]["t"])
    close(c2["icms"], 50)                 # base 200/0.8 = 250, 20 % = 50
    close(c2["credits_recovered"], 50)
    close(c2["total_cost"], 380)          # 200 + 80 + 100, ICMS fully credited
    # ex-tarifario switches the II rate
    f3 = json.loads(json.dumps(fake))
    l = f3["tiers"]["t"]["bom_lines"][0]
    l.update(ex_tarifario_applies=True, ii_rate_pct_ex_tarifario=0)
    close(landed_cost(f3, f3["tiers"]["t"])["ii"], 0)
    # unreachable target
    assert min_price(0.5, 100, 60) is None
    # validation reports TODOs
    bad = json.loads(json.dumps(fake))
    bad["fx_brl_per_usd"] = None
    bad["tiers"]["t"]["bom_lines"][1]["cost_brl"] = "TODO"
    errs = validate(bad)
    assert any("fx_brl_per_usd" in e for e in errs) and any("XOR" in e for e in errs), errs
    print("self-test OK")


# --------------------------------------------------------------------------- CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--inputs", help="filled inputs JSON (copy of margin_inputs.template.json)")
    ap.add_argument("--target-margin-pct", type=float, default=0.0,
                    help="gross margin %% of list price used for the minimum-viable-price output")
    ap.add_argument("--json", action="store_true", help="print machine-readable results")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    if not a.inputs:
        ap.error("--inputs is required (or use --self-test)")
    with open(a.inputs, encoding="utf-8") as fh:
        inp = json.load(fh)
    errs = validate(inp)
    if errs:
        print("Inputs incomplete. Fill these (null/TODO) fields before running:", file=sys.stderr)
        for e in errs:
            print("  -", e, file=sys.stderr)
        return 2
    res = run(inp, a.target_margin_pct)
    print(json.dumps(res, indent=2, default=str) if a.json else render(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
