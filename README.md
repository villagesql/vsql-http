# VillageSQL HTTP Extension

HTTP client functions for VillageSQL. Inspired by [pgsql-http](https://github.com/pramsey/pgsql-http), adapted for the VillageSQL Extension Framework (VEF).

## Features

- **HTTP methods**: GET, POST, PUT, DELETE, PATCH, and a generic `http_request()` function for any method with custom headers
- **JSON responses**: every function returns `{"status": N, "content_type": "...", "headers": [["name","value"],...], "content": "..."}`
- **URL encoding**: `url_encode()` and `url_decode()` for percent-encoding strings
- **Requires libcurl**: links the system libcurl (dev headers must be installed separately on Linux)

## Installation

If you installed VillageSQL with the install script, the Docker image, or a
release tarball, `vsql_http.veb` is already in the server's `lib/veb/`
directory — this extension is bundled with the server. There is nothing to build
or download:

```sql
INSTALL EXTENSION vsql_http;
```

Build from source only if you built the server from source without the bundled
extensions, or if you are working on this extension itself.

### Prerequisites

- VillageSQL build directory (specified via `VillageSQL_BUILD_DIR`)
- CMake 3.16 or higher
- C++17 compatible compiler
- libcurl development headers: `libcurl4-openssl-dev` (Ubuntu/Debian), `libcurl-devel` (RHEL/Fedora), or Xcode Command Line Tools (macOS)

📚 **Full Documentation**: Visit [villagesql.com/docs](https://villagesql.com/docs) for comprehensive guides on building extensions, architecture details, and more.

### Build Instructions

**Linux:**
```bash
mkdir build
cd build
cmake .. -DVillageSQL_BUILD_DIR=$HOME/build/villagesql
make -j $(getconf _NPROCESSORS_ONLN)
```

**macOS:**
```bash
mkdir build
cd build
cmake .. -DVillageSQL_BUILD_DIR="$HOME/build/villagesql"
make -j $(sysctl -n hw.logicalcpu)
```

This creates `vsql_http.veb` in the build directory.

### Install

```bash
make install
mysql -u root -e "INSTALL EXTENSION vsql_http;"
```

## Usage

### HTTP Requests

```sql
-- Simple GET
SELECT JSON_VALUE(
  CONVERT(vsql_http.http_get('https://api.example.com/data') USING utf8mb4),
  '$.status'
) AS status;

-- POST with JSON body
SELECT vsql_http.http_post(
  'https://api.example.com/items',
  'application/json',
  '{"name": "widget"}'
);

-- Any method with custom headers
SELECT vsql_http.http_request(
  'GET',
  'https://api.example.com/secure',
  '{"Authorization": "Bearer mytoken"}',
  NULL,
  NULL,
  NULL
);
```

### URL Encoding

```sql
SELECT vsql_http.url_encode('hello world & more');
-- → hello%20world%20%26%20more

SELECT vsql_http.url_decode('hello%20world%20%26%20more');
-- → hello world & more
```

### Syncing to External APIs via Triggers

HTTP functions work inside triggers, making it straightforward to push row changes to REST APIs or webhook endpoints.

```sql
DELIMITER $$
CREATE TRIGGER after_order_insert
  AFTER INSERT ON orders
  FOR EACH ROW
BEGIN
  SET @payload = JSON_OBJECT(
    'id',       NEW.id,
    'customer', NEW.customer_id,
    'total',    NEW.total
  );
  -- A NULL return means the request never completed. Always pass an explicit
  -- timeout, or a slow endpoint blocks the INSERT for as long as it hangs.
  SET @response = vsql_http.http_request(
    'POST', 'https://hooks.example.com/orders',
    NULL, @payload, 'application/json',
    '{"timeout": 5}'
  );
  IF @response IS NULL THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'order webhook failed';
  END IF;
END$$
DELIMITER ;
```

The call is synchronous — the trigger blocks until the request completes or times out. The options JSON carries the deadline:

```sql
SET @ignored = vsql_http.http_request(
  'POST', 'https://hooks.example.com/orders',
  NULL, @payload, 'application/json',
  '{"timeout": 5}'
);
```

### Extracting Response Fields

```sql
SET @resp = CONVERT(
  vsql_http.http_get('https://api.example.com/data') USING utf8mb4
);
SELECT
  JSON_VALUE(@resp, '$.status')       AS status,
  JSON_VALUE(@resp, '$.content_type') AS content_type,
  JSON_UNQUOTE(JSON_EXTRACT(@resp, '$.content')) AS body;
```

### Functions

| Function | Description |
|----------|-------------|
| `http_get(url)` | GET request |
| `http_post(url, content_type, body)` | POST request with body |
| `http_put(url, content_type, body)` | PUT request with body |
| `http_delete(url)` | DELETE request |
| `http_patch(url, content_type, body)` | PATCH request with body |
| `http_request(method, url, headers_json, body, content_type, options_json)` | Generic — any method, custom headers |
| `url_encode(text)` | Percent-encode a string |
| `url_decode(text)` | Decode a percent-encoded string |

All functions return NULL on connection failure or NULL input.

`http_request` accepts the standard methods in any case (`'get'` sends `GET`).
Any other method is sent exactly as given.

`url_encode` and `url_decode` are deterministic and can be used in generated columns
and CHECK constraints. The HTTP request functions are not, because they perform
network I/O — see [Known Limitations](#known-limitations).

### Response JSON Shape

Every HTTP function returns a JSON string with these fields:

| Field | Type | Description |
|-------|------|-------------|
| `status` | integer | HTTP status code |
| `content_type` | string | Value of the Content-Type response header |
| `headers` | array of `[name, value]` pairs | All response headers (names lowercased) |
| `content` | string | Response body |

Duplicate header names (e.g. `Set-Cookie`) appear as separate pairs in the array, preserving all values.

```sql
SET @resp = CONVERT(vsql_http.http_get('https://api.example.com/') USING utf8mb4);

-- Check a specific header
SELECT JSON_UNQUOTE(
  JSON_EXTRACT(@resp, '$.headers[0][1]')  -- value of first header
);

-- Count response headers
SELECT JSON_LENGTH(@resp, '$.headers');
```

## Testing

The MTR test suite (`mysql-test/`) covers urlencode/urldecode correctness and all HTTP
methods. HTTP tests run against a local `python3 -m http.server` instance — no
external network access required. A separate `vsql_http_live` test hits
`https://villagesql.com/robots.txt` to verify real HTTPS end-to-end; skip it
when offline with `--skip-test=vsql_http_live`.

### Option 1 (Default): Using installed VEB

**Linux:**
```bash
cd $HOME/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_http/mysql-test
```

**macOS:**
```bash
cd ~/build/villagesql/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql_http/mysql-test
```

### Option 2: Using a specific VEB file

**Linux:**
```bash
cd $HOME/build/villagesql/mysql-test
VSQL_HTTP_VEB=/path/to/vsql_http/build/vsql_http.veb \
  perl mysql-test-run.pl --suite=/path/to/vsql_http/mysql-test
```

**macOS:**
```bash
cd ~/build/villagesql/mysql-test
VSQL_HTTP_VEB=/path/to/vsql_http/build/vsql_http.veb \
  perl mysql-test-run.pl --suite=/path/to/vsql_http/mysql-test
```

See [TESTING.md](TESTING.md) for full instructions including result file regeneration.

## Development

```
vsql_http/
├── src/
│   └── vsql_http.cc        # VDF implementations and extension registration
├── cmake/
│   └── FindVillageSQL.cmake
├── mysql-test/
│   ├── suite.opt
│   ├── t/                  # MTR test files
│   └── r/                  # MTR expected results
├── manifest.json
├── CMakeLists.txt
└── build.sh
```

## Known Limitations

**Binary charset on 0.0.6 and earlier**: there, VEF STRING functions return
binary charset. Wrap with `CONVERT(... USING utf8mb4)` before passing to
`JSON_VALUE` or `JSON_EXTRACT`. Fixed in VillageSQL 0.0.7 — `CONVERT` is
harmless to keep either way, which is why every example above uses it.

**`JSON_VALUE` and large responses**: MySQL's `JSON_VALUE` returns NULL when the
extracted value exceeds its internal size limit. For responses with large bodies,
use `JSON_UNQUOTE(JSON_EXTRACT(...))` to read the content field.

**Response truncation**: The returned envelope is truncated at 262144 bytes
(256KB). The cut is at a byte boundary and can land inside the `content` string,
which leaves the value unparseable — every `JSON_*` function on it then raises
ERROR 3141 rather than returning a shortened body. Check
`LENGTH(response) < 262144` before parsing.

**HTTP functions are not deterministic**: `http_get`, `http_post`, `http_put`,
`http_delete`, `http_patch`, and `http_request` perform network I/O, so the server
rejects them in generated columns and CHECK constraints. Only `url_encode` and
`url_decode` may be used there.

```sql
CREATE TABLE feeds (u VARCHAR(255) NOT NULL, body TEXT AS (http_get(u)) STORED);
-- ERROR 3763 (HY000): Expression of generated column 'body' contains a
-- disallowed function: http_get.

CREATE TABLE feeds (u VARCHAR(255) NOT NULL, encoded TEXT AS (url_encode(u)) STORED);
-- OK: 'a b&c' -> 'a%20b%26c'
```

**No function overloading**: VEF does not support multiple signatures for the same
function name. To pass custom headers, use the generic `http_request()` function
instead of `http_get()`.
→ [Open issue](https://github.com/villagesql/villagesql-server/issues/new?title=VEF:+support+function+overloading+%28multiple+VDF+signatures+with+same+name%2C+different+arity%29)

**`alt_str_buf` not populated**: VEF's `alt_str_buf` field in `vef_vdf_result_t`
is designed for variable-length zero-copy string returns, but the server never
populates it. All results go through the fixed `str_buf`, causing the 256KB
truncation limit.
→ [Open issue](https://github.com/villagesql/villagesql-server/issues/new?title=VEF:+implement+alt_str_buf+in+vef_vdf_result_t+for+variable-length+zero-copy+string+returns)

## Reporting Bugs and Requesting Features

Open an [issue on GitHub](https://github.com/villagesql/villagesql-server/issues). Include a description, steps to reproduce, and your VillageSQL version (`SELECT @@villagesql_server_version`).

## License

GPL v2 — see LICENSE.

## Contributing

See the [VillageSQL Contributing Guide](https://github.com/villagesql/villagesql-server/blob/main/CONTRIBUTING.md).

## Contact

- [Discord](https://discord.gg/KSr6whd3Fr)
- [GitHub Issues](https://github.com/villagesql/villagesql-server/issues)
- [GitHub Discussions](https://github.com/villagesql/villagesql-server/discussions)
