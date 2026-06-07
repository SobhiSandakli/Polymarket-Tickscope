# Bot-only AWS setup

> **Status:** reference only. The execution infrastructure is built and paper-tested but
> not deployed — no strategy survived out-of-sample validation. See `docs/FINDINGS.md`.

This guide deploys only `polymarket_bot` on EC2 and uploads one daily bot artifact to S3.

## 1) Build bot on EC2

```bash
cd /opt/polymarket
cmake -S . -B build
cmake --build build --target polymarket_bot -j"$(nproc)"
```

If you still see `No rule to make target 'polymarket_bot'`, your cache is stale:

```bash
cd /opt/polymarket
rm -f build/CMakeCache.txt
cmake -S . -B build
cmake --build build --target polymarket_bot -j"$(nproc)"
```

## 2) Install systemd service

```bash
sudo cp deploy/systemd/polymarket-bot.service /etc/systemd/system/
sudo cp deploy/systemd/polymarket-bot.env.example /etc/default/polymarket-bot
sudo nano /etc/default/polymarket-bot
```

Set your values in `/etc/default/polymarket-bot`, then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now polymarket-bot
sudo systemctl status polymarket-bot --no-pager
```

Useful logs:

```bash
journalctl -u polymarket-bot -f
```

## 3) Configure daily S3 artifact upload

`scripts/bot_daily_s3.sh` creates:

- `bot_trades_YYYY-MM-DD.log.gz` from `paper_trades.log`
- `bot_status_YYYY-MM-DD.log.gz` from `paper_status.log` (cash/equity snapshots)
- uploads to `s3://.../daily/date=YYYY-MM-DD/`

Test once manually:

```bash
export BOT_LOG_PATH=/opt/polymarket/paper_trades.log
export BOT_STATUS_LOG_PATH=/opt/polymarket/paper_status.log
export BOT_ARCHIVE_DIR=/opt/polymarket/bot_archive
export BOT_S3_URI=s3://polymarket-data-sobhi/polymarket-bot
/opt/polymarket/scripts/bot_daily_s3.sh
```

Add cron (runs 00:10 UTC daily, uploads yesterday):

```bash
(crontab -l 2>/dev/null; echo '10 0 * * * BOT_LOG_PATH=/opt/polymarket/paper_trades.log BOT_STATUS_LOG_PATH=/opt/polymarket/paper_status.log BOT_ARCHIVE_DIR=/opt/polymarket/bot_archive BOT_S3_URI=s3://YOUR_BUCKET/polymarket-bot /opt/polymarket/scripts/bot_daily_s3.sh >> /var/log/polymarket_bot_daily.log 2>&1') | crontab -
```

## 4) Download daily file for analysis

```bash
aws s3 cp s3://polymarket-data-sobhi/polymarket-bot/daily/date=YYYY-MM-DD/bot_trades_YYYY-MM-DD.log.gz .
aws s3 cp s3://polymarket-data-sobhi/polymarket-bot/daily/date=YYYY-MM-DD/bot_status_YYYY-MM-DD.log.gz .
gunzip -f bot_trades_YYYY-MM-DD.log.gz
gunzip -f bot_status_YYYY-MM-DD.log.gz
```

Send the extracted `.log` to Copilot for analysis.
