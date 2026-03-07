# Binance Harvester — AWS Setup

Runs alongside the Polymarket harvester on the same EC2 instance.
Collects Binance BTCUSDT bookTicker quotes with ms-precision timestamps.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target binance_harvester -j$(nproc)
```

## Deploy

```bash
# Copy service file
sudo cp deploy/binance/binance-harvester.service /etc/systemd/system/
sudo systemctl daemon-reload

# Enable and start
sudo systemctl enable binance-harvester
sudo systemctl start binance-harvester
sudo journalctl -u binance-harvester -f
```

## Data Output

Binary journals land in `$BINANCE_DATA_DIR` (default: `/opt/polymarket/data`):
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
sudo journalctl -u binance-harvester --since "5 min ago"

# Spot-check a record
python3 -c "
import struct, sys
with open('/opt/polymarket/data/btc_$(date -u +%Y%m%d_%H)*.bin', 'rb') as f:
    d = f.read(64)
ts, bid, ask, mid = struct.unpack('<Qddd32x', d)
print(f'ts={ts} bid={bid} ask={ask} mid={mid}')
"
```
