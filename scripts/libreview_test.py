#!/usr/bin/env python3
"""Minimal LibreLinkUp API smoke test.

Reads credentials from CLI flags or environment variables and prints the latest
measurement. No data is stored.

Environment variables (fallbacks for CLI):
  LIBRE_EMAIL, LIBRE_PASSWORD, LIBRE_REGION (default: eu)

Usage examples:
  python libreview_test.py --email you@example.com --password "Secret" --region eu
  LIBRE_EMAIL=you@example.com LIBRE_PASSWORD=Secret python libreview_test.py
"""
import argparse
import hashlib
import os
import sys
from typing import Dict, Optional, Tuple

import requests

PRODUCT = "llu.android"
VERSION = "4.16.0"
DEFAULT_REGION = "eu"
BASE_TEMPLATE = "https://api-{region}.libreview.io"

# Hardcoded defaults; replace with your own credentials for quick testing
HARDCODED_EMAIL = ""
HARDCODED_PASSWORD = ""
HARDCODED_REGION = DEFAULT_REGION


def sha256_hex(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def build_headers(token: Optional[str] = None, account_id: Optional[str] = None) -> Dict[str, str]:
    headers = {
        "product": PRODUCT,
        "version": VERSION,
        "accept-encoding": "gzip",
        "cache-control": "no-cache",
        "connection": "keep-alive",
        "content-type": "application/json",
        "user-agent": "LibreLinkUpTest/1.0",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if account_id:
        headers["Account-Id"] = account_id
    return headers


def login(base_url: str, email: str, password: str) -> Tuple[str, str, str]:
    # Handle possible redirect to region-specific host
    resp = requests.post(
        f"{base_url}/llu/auth/login",
        headers=build_headers(),
        json={"email": email, "password": password},
        timeout=20,
    )
    resp.raise_for_status()
    data = resp.json()
    status = data.get("status")
    if status == 0 and data.get("data", {}).get("redirect"):
        region = data.get("data", {}).get("region")
        new_base = region if region and region.startswith("http") else BASE_TEMPLATE.format(region=region)
        resp = requests.post(
            f"{new_base}/llu/auth/login",
            headers=build_headers(),
            json={"email": email, "password": password},
            timeout=20,
        )
        resp.raise_for_status()
        data = resp.json()
        status = data.get("status")
        base_url = new_base

    if status != 0:
        raise RuntimeError(f"Login failed with status {status}: {data}")
    token = data.get("data", {}).get("authTicket", {}).get("token")
    user_id = data.get("data", {}).get("user", {}).get("id")
    if not token or not user_id:
        raise RuntimeError("Login response missing token or user id")
    return token, user_id, base_url


def get_connections(base_url: str, token: str, account_id: str):
    resp = requests.get(
        f"{base_url}/llu/connections",
        headers=build_headers(token, account_id),
        timeout=20,
    )
    resp.raise_for_status()
    data = resp.json()
    status = data.get("status")
    if status != 0:
        raise RuntimeError(f"Connections failed with status {status}: {data}")
    items = data.get("data") or []
    if not items:
        raise RuntimeError("No connections returned")
    return items[0]


def get_graph_latest(base_url: str, token: str, account_id: str, patient_id: str):
    resp = requests.get(
        f"{base_url}/llu/connections/{patient_id}/graph",
        headers=build_headers(token, account_id),
        timeout=20,
    )
    resp.raise_for_status()
    data = resp.json()
    status = data.get("status")
    if status != 0:
        raise RuntimeError(f"Graph failed with status {status}: {data}")
    conn = (data.get("data") or {}).get("connection") or {}
    # Prefer measurementData if present, else glucoseMeasurement/glucoseItem
    measurement = conn.get("glucoseMeasurement") or conn.get("glucoseItem") or {}
    series = measurement.get("measurementData") or []
    latest = series[-1] if series else measurement
    return latest


def main():
    parser = argparse.ArgumentParser(description="LibreLinkUp API smoke test")
    parser.add_argument(
        "--email",
        default=HARDCODED_EMAIL or os.getenv("LIBRE_EMAIL"),
        help="LibreLinkUp email",
    )
    parser.add_argument(
        "--password",
        default=HARDCODED_PASSWORD or os.getenv("LIBRE_PASSWORD"),
        help="LibreLinkUp password",
    )
    parser.add_argument(
        "--region",
        default=HARDCODED_REGION or os.getenv("LIBRE_REGION", DEFAULT_REGION),
        help="Region code (eu/us/ae/etc)",
    )
    args = parser.parse_args()

    if not args.email or not args.password:
        print("Email and password are required (via args or environment)", file=sys.stderr)
        sys.exit(1)

    base_url = BASE_TEMPLATE.format(region=args.region)
    print(f"Using base URL: {base_url}")

    token, user_id, base_url = login(base_url, args.email, args.password)
    account_id = sha256_hex(user_id)
    print("Login ok; token obtained")

    conn = get_connections(base_url, token, account_id)
    patient_id = conn.get("patientId")
    print(f"Connections ok; patient: {patient_id}")

    measurement = get_graph_latest(base_url, token, account_id, patient_id)
    value = measurement.get("ValueInMgPerDl") or measurement.get("Value")
    trend = measurement.get("TrendArrow") or measurement.get("Trend")
    ts = measurement.get("Timestamp") or measurement.get("FactoryTimestamp")
    print(f"Latest: value={value}, trend={trend}, timestamp={ts}")


if __name__ == "__main__":
    main()
