#!/usr/bin/env python3
"""
AgentOS × Corax — 股票数据拉取工具

从 tushare 获取 A 股行情数据，输出 Corax 兼容的 JSON 格式到 stdout。
支持日线、分钟线，支持股票代码/名称模糊匹配。

依赖:
    pip install tushare pandas

环境变量:
    TUSHARE_TOKEN — tushare API token (必须)

用法:
    # 日线数据
    python fetch_stock.py --code 600519 --period daily --limit 30

    # 分钟线
    python fetch_stock.py --code 600519 --period 5min --limit 60

    # 按名称搜索
    python fetch_stock.py --name 茅台 --period daily --limit 10

    # 多只股票
    python fetch_stock.py --code 600519,000858 --period daily --limit 10

    # 列出可用股票（搜索）
    python fetch_stock.py --search 新能源

输出格式 (Corax 兼容):
    {"status":"ok","data":[{"ts":"2024-01-15T10:30:00.000000000Z","symbol":"600519","open":1800.0,...}],"schema":[...]}
"""

import argparse
import json
import os
import sys
from datetime import datetime, timedelta

def ensure_tushare():
    """确保 tushare 可用"""
    try:
        import tushare as ts
        token = os.environ.get("TUSHARE_TOKEN", "")
        if not token:
            return None, "TUSHARE_TOKEN environment variable not set"
        ts.set_token(token)
        pro = ts.pro_api()
        return pro, None
    except ImportError:
        return None, "tushare not installed. Run: pip install tushare"
    except Exception as e:
        return None, f"tushare init failed: {e}"


def normalize_code(code: str) -> str:
    """标准化股票代码: 600519 → 600519.SH, 000858 → 000858.SZ"""
    code = code.strip()
    if "." in code:
        return code.upper()
    if code.startswith("6"):
        return code + ".SH"
    elif code.startswith("0") or code.startswith("3"):
        return code + ".SZ"
    elif code.startswith("8") or code.startswith("4"):
        return code + ".BJ"
    return code + ".SH"


def to_corax_timestamp(date_str: str, time_str: str = None) -> str:
    """转换为 Corax ISO8601 纳秒时间戳"""
    if time_str:
        dt = datetime.strptime(f"{date_str} {time_str}", "%Y%m%d %H:%M:%S")
    else:
        dt = datetime.strptime(str(date_str)[:10].replace("-", ""), "%Y%m%d")
    return dt.strftime("%Y-%m-%dT%H:%M:%S.000000000Z")


def fetch_daily(pro, codes: list[str], limit: int, start_date: str = None, end_date: str = None):
    """拉取日线数据"""
    import pandas as pd

    if not end_date:
        end_date = datetime.now().strftime("%Y%m%d")
    if not start_date:
        start_date = (datetime.now() - timedelta(days=limit * 2)).strftime("%Y%m%d")

    all_rows = []
    schema = [
        {"name": "ts", "type": "timestamp"},
        {"name": "symbol", "type": "string"},
        {"name": "open", "type": "float64"},
        {"name": "high", "type": "float64"},
        {"name": "low", "type": "float64"},
        {"name": "close", "type": "float64"},
        {"name": "volume", "type": "float64"},
        {"name": "amount", "type": "float64"},
        {"name": "change_pct", "type": "float64"},
    ]

    for code in codes:
        ts_code = normalize_code(code)
        try:
            df = pro.daily(ts_code=ts_code, start_date=start_date, end_date=end_date)
            if df is None or df.empty:
                continue

            df = df.sort_values("trade_date").tail(limit)

            for _, row in df.iterrows():
                all_rows.append({
                    "ts": to_corax_timestamp(row["trade_date"]),
                    "symbol": ts_code.split(".")[0],
                    "open": round(float(row["open"]), 2),
                    "high": round(float(row["high"]), 2),
                    "low": round(float(row["low"]), 2),
                    "close": round(float(row["close"]), 2),
                    "volume": round(float(row["vol"]), 0),
                    "amount": round(float(row["amount"]), 0),
                    "change_pct": round(float(row["pct_chg"]), 4),
                })
        except Exception as e:
            sys.stderr.write(f"Warning: failed to fetch {ts_code}: {e}\n")

    return all_rows, schema


def fetch_minute(pro, codes: list[str], period: str, limit: int):
    """拉取分钟线数据"""
    import tushare as ts

    freq_map = {"1min": "1min", "5min": "5min", "15min": "15min", "30min": "30min", "60min": "60min"}
    freq = freq_map.get(period, "5min")

    all_rows = []
    schema = [
        {"name": "ts", "type": "timestamp"},
        {"name": "symbol", "type": "string"},
        {"name": "open", "type": "float64"},
        {"name": "high", "type": "float64"},
        {"name": "low", "type": "float64"},
        {"name": "close", "type": "float64"},
        {"name": "volume", "type": "float64"},
        {"name": "amount", "type": "float64"},
    ]

    for code in codes:
        ts_code = normalize_code(code)
        try:
            df = ts.pro_bar(ts_code=ts_code, freq=freq, adj="qfq")
            if df is None or df.empty:
                continue

            df = df.sort_values("trade_time" if "trade_time" in df.columns else "trade_date").tail(limit)

            time_col = "trade_time" if "trade_time" in df.columns else "trade_date"
            for _, row in df.iterrows():
                ts_val = str(row[time_col])
                # 分钟线 trade_time 格式: "2024-01-15 10:30:00"
                if " " in ts_val:
                    dt = datetime.strptime(ts_val, "%Y-%m-%d %H:%M:%S")
                    ts_str = dt.strftime("%Y-%m-%dT%H:%M:%S.000000000Z")
                else:
                    ts_str = to_corax_timestamp(ts_val)

                all_rows.append({
                    "ts": ts_str,
                    "symbol": ts_code.split(".")[0],
                    "open": round(float(row["open"]), 2),
                    "high": round(float(row["high"]), 2),
                    "low": round(float(row["low"]), 2),
                    "close": round(float(row["close"]), 2),
                    "volume": round(float(row.get("vol", 0)), 0),
                    "amount": round(float(row.get("amount", 0)), 0),
                })
        except Exception as e:
            sys.stderr.write(f"Warning: failed to fetch {ts_code} ({freq}): {e}\n")

    return all_rows, schema


def search_stocks(pro, keyword: str, limit: int = 20):
    """搜索股票名称/代码"""
    try:
        df = pro.stock_basic(exchange="", list_status="L",
                             fields="ts_code,symbol,name,industry,area")
        if df is None or df.empty:
            return []

        mask = (df["name"].str.contains(keyword, na=False) |
                df["symbol"].str.contains(keyword, na=False) |
                df["ts_code"].str.contains(keyword, na=False) |
                df["industry"].str.contains(keyword, na=False))

        results = df[mask].head(limit)
        return [
            {"code": row["symbol"], "ts_code": row["ts_code"],
             "name": row["name"], "industry": row.get("industry", "")}
            for _, row in results.iterrows()
        ]
    except Exception as e:
        return [{"error": str(e)}]


def main():
    parser = argparse.ArgumentParser(description="Fetch stock data for Corax")
    parser.add_argument("--code", help="Stock code(s), comma-separated, e.g. 600519,000858")
    parser.add_argument("--name", help="Stock name (fuzzy match)")
    parser.add_argument("--search", help="Search stocks by keyword")
    parser.add_argument("--period", default="daily",
                        choices=["daily", "1min", "5min", "15min", "30min", "60min"],
                        help="Data period (default: daily)")
    parser.add_argument("--limit", type=int, default=30, help="Max rows per stock (default: 30)")
    parser.add_argument("--start", help="Start date (YYYYMMDD)")
    parser.add_argument("--end", help="End date (YYYYMMDD)")
    args = parser.parse_args()

    pro, err = ensure_tushare()
    if err:
        print(json.dumps({"status": "error", "error": err}))
        sys.exit(1)

    # 搜索模式
    if args.search:
        results = search_stocks(pro, args.search, args.limit)
        print(json.dumps({"status": "ok", "stocks": results}, ensure_ascii=False))
        return

    # 确定股票代码
    codes = []
    if args.code:
        codes = [c.strip() for c in args.code.split(",")]
    elif args.name:
        # 按名称查找
        matches = search_stocks(pro, args.name, 1)
        if matches and "code" in matches[0]:
            codes = [matches[0]["code"]]
        else:
            print(json.dumps({"status": "error",
                              "error": f"Stock not found: {args.name}"}))
            sys.exit(1)

    if not codes:
        print(json.dumps({"status": "error",
                          "error": "Provide --code, --name, or --search"}))
        sys.exit(1)

    # 拉取数据
    if args.period == "daily":
        data, schema = fetch_daily(pro, codes, args.limit, args.start, args.end)
    else:
        data, schema = fetch_minute(pro, codes, args.period, args.limit)

    result = {
        "status": "ok",
        "count": len(data),
        "schema": schema,
        "data": data,
    }
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
