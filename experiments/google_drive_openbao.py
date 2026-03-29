"""Demonstrate Google Drive API access with OpenBao-managed OAuth tokens.

1. Fetch Google OAuth credentials (client_id, client_secret, refresh_token)
   from OpenBao KV v2 at secret/blobapi/google.
2. Exchange the refresh token for a fresh access token via Google's token endpoint.
3. Use DuckDB + blobhttp to list files in a shared Google Drive folder,
   passing the Bearer token directly in the bh_http_get headers.
"""

import json
import requests
import duckdb

VAULT_ADDR = "http://127.0.0.1:8200"
VAULT_TOKEN = "dev-blobapi-token"
VAULT_SECRET_PATH = "secret/data/blobapi/google"

BHTTP_EXT = "/Users/paulharrington/checkouts/blobhttp/build/release/bhttp.duckdb_extension"

FOLDER_ID = "1C01bJDPMZfChCJhgUd11W9eF3HVqXjYT"
QUOTA_PROJECT = "meplex-integration"


def fetch_google_creds_from_vault():
    """Read Google OAuth credentials from OpenBao KV v2."""
    resp = requests.get(
        f"{VAULT_ADDR}/v1/{VAULT_SECRET_PATH}",
        headers={"X-Vault-Token": VAULT_TOKEN},
    )
    resp.raise_for_status()
    data = resp.json()["data"]["data"]
    return (
        data["client_id"],
        data["client_secret"],
        data["refresh_token"],
        data["token_uri"],
        data.get("quota_project", "meplex-integration"),
    )


def exchange_refresh_token(client_id, client_secret, refresh_token, token_uri):
    """Exchange a refresh token for a fresh access token."""
    resp = requests.post(
        token_uri,
        data={
            "client_id": client_id,
            "client_secret": client_secret,
            "refresh_token": refresh_token,
            "grant_type": "refresh_token",
        },
    )
    resp.raise_for_status()
    token_data = resp.json()
    return token_data["access_token"], token_data.get("expires_in", 3600)


def main():
    # Step 1: Get credentials from OpenBao
    print("Fetching Google OAuth credentials from OpenBao...")
    client_id, client_secret, refresh_token, token_uri, quota_project = fetch_google_creds_from_vault()
    print(f"  client_id: {client_id[:20]}...")

    # Step 2: Exchange for a fresh access token
    print("Exchanging refresh token for access token...")
    access_token, expires_in = exchange_refresh_token(client_id, client_secret, refresh_token, token_uri)
    print(f"  access_token: {access_token[:20]}... (expires in {expires_in}s)")

    # Step 3: Use DuckDB + blobhttp to list files in the Drive folder
    print(f"\nListing files in Google Drive folder {FOLDER_ID}...\n")

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BHTTP_EXT}'")

    # Drive API v3: list files in folder, passing Bearer token in headers
    url = (
        f"https://www.googleapis.com/drive/v3/files"
        f"?q=%27{FOLDER_ID}%27+in+parents"
        f"&fields=files(id,name,mimeType,modifiedTime,size)"
        f"&pageSize=100"
        f"&includeItemsFromAllDrives=true"
        f"&supportsAllDrives=true"
    )

    result = con.execute(
        """
        SELECT r.response_status_code,
               r.response_body
        FROM (
            SELECT bh_http_get(
                $1,
                headers := MAP {
                    'Authorization': 'Bearer ' || $2,
                    'X-Goog-User-Project': $3
                }
            ) AS r
        )
        """,
        [url, access_token, quota_project],
    ).fetchone()

    status_code = result[0]
    body = result[1]

    if status_code != 200:
        print(f"ERROR: HTTP {status_code}")
        print(body[:500])
        return

    files = json.loads(body).get("files", [])
    print(f"{'Name':<50} {'Type':<45} {'Modified':<25} {'Size':>10}")
    print("-" * 135)
    for f in files:
        print(
            f"{f['name']:<50} "
            f"{f['mimeType']:<45} "
            f"{f.get('modifiedTime', ''):<25} "
            f"{f.get('size', ''):>10}"
        )
    print(f"\n{len(files)} file(s) found.")


if __name__ == "__main__":
    main()
