#!/usr/bin/env python3
"""Root-free TCP throttling proxy.

Sits between WiiFin (running in Dolphin) and a real Jellyfin server, rate-limiting
traffic in both directions to approximate a real Wii's slow 802.11b wifi. No sudo
needed -- point WiiFin's saved profile server_url at this proxy's host:port instead
of the real server, and this forwards the connection while capping throughput.

Usage:
    python3 throttle_proxy.py --listen-host 192.168.50.189 --listen-port 18096 \
        --target-host blair.crabdance.com --target-port 8096 --kbps 256 \
        --log-file requests.log

--kbps applies per direction (upload and download each capped independently).
Every proxied HTTP request is logged (method, path, byte counts, duration) to
both stdout and --log-file. Kill with Ctrl-C.
"""
import argparse
import asyncio
import logging
import time


class TokenBucket:
    """Simple token bucket: `rate` bytes/sec, refilled continuously."""

    def __init__(self, rate_bytes_per_sec: float):
        self.rate = rate_bytes_per_sec
        self.tokens = 0.0  # start empty -- no free burst credit
        self.last = time.monotonic()

    async def consume(self, n: int):
        while True:
            now = time.monotonic()
            elapsed = now - self.last
            self.last = now
            self.tokens = min(self.rate, self.tokens + elapsed * self.rate)
            if self.tokens >= n:
                self.tokens -= n
                return
            deficit = n - self.tokens
            await asyncio.sleep(deficit / self.rate)


class Counter:
    def __init__(self):
        self.bytes = 0


async def pump(reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
               bucket: TokenBucket, counter: Counter, on_first_chunk=None):
    first = True
    try:
        while True:
            chunk = await reader.read(4096)
            if not chunk:
                break
            if first and on_first_chunk:
                on_first_chunk(chunk)
            first = False
            await bucket.consume(len(chunk))
            counter.bytes += len(chunk)
            writer.write(chunk)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()


def parse_request_line(chunk: bytes) -> str:
    try:
        line = chunk.split(b"\r\n", 1)[0].decode("latin-1")
        return line if line else "(empty)"
    except Exception:
        return "(unparseable)"


async def handle_client(client_reader, client_writer, target_host, target_port, kbps, log):
    peer = client_writer.get_extra_info("peername")
    start = time.monotonic()
    try:
        server_reader, server_writer = await asyncio.open_connection(target_host, target_port)
    except OSError as e:
        log.info(f"CONNECT-FAIL peer={peer} target={target_host}:{target_port} error={e}")
        client_writer.close()
        return

    rate = kbps * 1024 / 8  # kilobits/sec -> bytes/sec
    up_bucket = TokenBucket(rate)
    down_bucket = TokenBucket(rate)
    up_counter = Counter()
    down_counter = Counter()
    request_line = {"value": None}

    def capture_request(chunk: bytes):
        request_line["value"] = parse_request_line(chunk)

    await asyncio.gather(
        pump(client_reader, server_writer, up_bucket, up_counter, on_first_chunk=capture_request),
        pump(server_reader, client_writer, down_bucket, down_counter),
    )
    duration = time.monotonic() - start
    up_kb = up_counter.bytes / 1024
    down_kb = down_counter.bytes / 1024
    log.info(f"peer={peer} request={request_line['value']!r} "
             f"up={up_kb:.2f}KB down={down_kb:.2f}KB duration={duration:.2f}s")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-host", default="0.0.0.0")
    ap.add_argument("--listen-port", type=int, required=True)
    ap.add_argument("--target-host", required=True)
    ap.add_argument("--target-port", type=int, required=True)
    ap.add_argument("--kbps", type=float, default=2048,
                     help="throughput cap per direction, in kilobits/sec (default 2048 = 2 Mbps)")
    ap.add_argument("--log-file", default=None,
                     help="path to write a per-request log (in addition to stdout)")
    args = ap.parse_args()

    log = logging.getLogger("throttle_proxy")
    log.setLevel(logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    stream_handler = logging.StreamHandler()
    stream_handler.setFormatter(fmt)
    log.addHandler(stream_handler)
    if args.log_file:
        file_handler = logging.FileHandler(args.log_file)
        file_handler.setFormatter(fmt)
        log.addHandler(file_handler)

    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, args.target_host, args.target_port, args.kbps, log),
        args.listen_host, args.listen_port,
    )
    log.info(f"listening on {args.listen_host}:{args.listen_port} -> "
             f"{args.target_host}:{args.target_port} capped at {args.kbps} kbps/direction"
             + (f", logging requests to {args.log_file}" if args.log_file else ""))
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
