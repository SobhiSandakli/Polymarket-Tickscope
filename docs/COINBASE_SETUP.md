# Coinbase Harvester — AWS Setup

Runs alongside the Polymarket harvester on the same EC2 instance.
Collects Coinbase BTC-USD ticker quotes with ms-precision timestamps.

> Why Coinbase, not Binance? Binance.com returns HTTP 451 (geo-blocked) from
> US-region AWS IPs. Coinbase is US-domiciled and accessible from any AWS region;
> BTC-USD liquidity is equivalent for this lag-research purpose.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target coinbase_harvester -j$(nproc)
```

## Deploy

```bash
# Copy service file
sudo cp deploy/coinbase/coinbase-harvester.service /etc/systemd/system/
sudo systemctl daemon-reload

# Enable and start
sudo systemctl enable coinbase-harvester
sudo systemctl start coinbase-harvester
sudo journalctl -u coinbase-harvester -f
```

## Data Output

Binary journals land in `$COINBASE_DATA_DIR` (default: `/opt/polymarket/data`):
```
btc_20260307_0930.bin   # 15-min rotation, same as Polymarket harvester
btc_20260307_0945.bin
...
```

`hourly_flush.sh` converts these to Parquet and uploads to S3 automatically.

## Manual Parquet Conversion

```bash
python3 scripts/harvester/btc_to_parquet.py btc_20260307_0930.bin
```

## Verify It's Working

```bash
# Check journal file is growing
ls -la /opt/polymarket/data/btc_*.bin

# Check logs
sudo journalctl -u coinbase-harvester --since "5 min ago"

# Spot-check a record
python3 -c "
import struct, sys
with open('/opt/polymarket/data/btc_$(date -u +%Y%m%d_%H)*.bin', 'rb') as f:
    d = f.read(64)
ts, bid, ask, mid = struct.unpack('<Qddd32x', d)
print(f'ts={ts} bid={bid} ask={ask} mid={mid}')
"
```
