# Testing vsql_http

## Run the test suite

Tests use MTR (MySQL Test Runner). Run from the server's `mysql-test/` directory:

```bash
cd /path/to/villagesql/build/mysql-test

perl mysql-test-run.pl --suite=/path/to/vsql_http/test
```

All 3 tests should pass offline:
- `vsql_http_encode` — urlencode/urldecode correctness and NULL handling
- `vsql_http_requests` — all HTTP methods against a local Python HTTP server

A fourth test requires network access:
- `vsql_http_live` — GET `https://villagesql.com/robots.txt`, verifies real HTTPS works end-to-end

Skip it when offline:
```bash
perl mysql-test-run.pl --suite=/path/to/vsql_http/test --skip-test=vsql_http_live
```

## Regenerate result files

If you change the extension and the expected output changes:

```bash
perl mysql-test-run.pl --suite=/path/to/vsql_http/test --record
```

## What the tests cover

**vsql_http_encode.test**
- `urlencode('hello world')` → `hello%20world`
- `urlencode('a=1&b=2')` → `a%3D1%26b%3D2`
- Empty string, no-encoding-needed string, full URL with spaces
- `urldecode` inverses
- Round-trip: `urldecode(urlencode(s)) = s`
- NULL input → NULL return

**vsql_http_requests.test**
- Starts a Python HTTP server on port 18777 serving `$MYSQLTEST_VARDIR`
- `http_get` → status 200, content_type present
- `http()` generic GET → status 200
- `http()` with custom header → status 200
- `http_post`, `http_delete`, `http_put`, `http_patch` → each returns a status code
- NULL URL → NULL return
- Unreachable host → NULL return (no crash)
- Tears down the HTTP server with `pkill` after the test

## Notes

**Binary charset**: VEF STRING return type produces binary charset. All test queries
wrap function output in `CONVERT(... USING utf8mb4)` before passing to `JSON_VALUE`.

**`JSON_VALUE` size limit**: MySQL's `JSON_VALUE` returns NULL for extracted values
larger than its internal limit (~512 chars). The `content` field of a real HTTP
response is often larger. Tests check `content_type IS NOT NULL` and `status`
rather than the raw body content to avoid this. Use `JSON_EXTRACT` for body content.
