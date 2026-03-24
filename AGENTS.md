# AGENTS.md

This file provides guidance to AI coding assistants (Claude Code, Gemini Code Assist, etc.) when working with code in this repository.

**Note**: Also check `AGENTS.local.md` for additional local development instructions when present.

## Project Overview

This is an HTTP client extension for VillageSQL (a MySQL-compatible database) that provides HTTP request functions and URL encoding/decoding. Inspired by [pgsql-http](https://github.com/pramsey/pgsql-http), the extension is built as a shared library (.so) packaged in a VEB (VillageSQL Extension Bundle) archive for installation.

## Build System

**IMPORTANT**: Always build in the `build/` directory, never in the source root. Building in the source root creates files that should not be checked into git.

### Build Instructions

1. Configure CMake with required paths:
   ```bash
   mkdir build
   cd build
   cmake .. -DVillageSQL_BUILD_DIR=/path/to/villagesql/build
   ```

   **Note**:
   - `VillageSQL_BUILD_DIR`: Path to VillageSQL build directory (contains the staged SDK and `veb_output_directory`)

2. Build the extension:
   ```bash
   make
   ```

3. Install the VEB (optional):
   ```bash
   make install
   ```

The build process:
1. Uses CMake with the VillageSQL Extension Framework SDK
2. Compiles C++ source files into shared library `vsql_http.so`
3. Packages library with `manifest.json` into `vsql_http.veb` package using `VEF_CREATE_VEB()` macro
4. VEB can be installed to VillageSQL for use
5. libcurl is linked during build (system libcurl, no external dependency)

See `AGENTS.local.md` for machine-specific build paths and configurations.

## Architecture

**Core Components:**
- `src/vsql_http.cc` - All VDF (VillageSQL Defined Function) implementations, curl helpers, and extension registration via `VEF_GENERATE_ENTRY_POINTS()`
- `manifest.json` - Extension metadata (name, version, description, author, license)
- `CMakeLists.txt` - CMake build configuration
- `cmake/FindVillageSQL.cmake` - CMake module to locate VillageSQL SDK
- `test/t/` - Test files directory (`.test` files using MTR framework)
- `test/r/` - Expected test results directory (`.result` files)

**Available Functions:**
- `http_get(url)` - GET request
- `http_post(url, content_type, body)` - POST request with body
- `http_put(url, content_type, body)` - PUT request with body
- `http_delete(url)` - DELETE request
- `http_patch(url, content_type, body)` - PATCH request with body
- `http_request(method, url, headers_json, body, content_type, options_json)` - Generic request with custom headers and options
- `url_encode(text)` - Percent-encode a string
- `url_decode(text)` - Decode a percent-encoded string

**Response JSON Shape:**
All HTTP functions return a JSON string: `{"status": N, "content_type": "...", "headers": [["name","value"],...], "content": "..."}`

All functions return NULL on connection failure or NULL input.

**Error Handling:**
- HTTP functions return NULL on curl-level failure (connection refused, DNS failure, timeout)
- `url_encode`/`url_decode` return NULL for NULL input or curl init failure
- Exceptions are caught and returned as `VEF_RESULT_ERROR`

**Key Implementation Details:**
- Thread-local curl handles with connection keep-alive (`curl_easy_reset()` between calls)
- Process-wide `curl_global_init()` via `std::once_flag`
- HTTP buffer size: 256KB (responses exceeding this are truncated)
- URL encode/decode buffer size: 8KB
- Header names are lowercased per HTTP/2 convention
- Custom headers parsed from JSON object format `{"Key": "Value", ...}`
- Options JSON supports: `timeout` (int), `proxy`, `user_agent`, `ssl_cert`, `ssl_key`, `ssl_ca_bundle` (strings)

**Dependencies:**
- VillageSQL Extension Framework SDK (`<villagesql/extension.h>`)
- libcurl (system-provided)

**Code Organization:**
- File naming: lowercase with underscores (e.g., `vsql_http.cc`)
- Function naming: lowercase with underscores (e.g., `http_get_1_impl`)
- Variable naming: lowercase with underscores (e.g., `response_body`)

## VillageSQL Extension Framework (VEF) API Pattern

### Function Implementation Pattern

```cpp
void my_function_impl(vef_context_t* ctx,
                      vef_invalue_t* arg1, vef_invalue_t* arg2,
                      vef_vdf_result_t* result) {
    // Check for NULL arguments
    if (arg1->is_null || arg2->is_null) {
        result->type = VEF_RESULT_NULL;
        return;
    }

    // Access argument values
    const char* str_value = arg1->str_value;
    size_t str_len = arg1->str_len;

    // Perform function logic
    // ...

    // Set result
    result->type = VEF_RESULT_VALUE;
    result->actual_len = result_length;
    // Write to result->str_buf
}
```

### Function Registration Pattern

```cpp
#include <villagesql/extension.h>

using namespace villagesql::extension_builder;

VEF_GENERATE_ENTRY_POINTS(
  make_extension("extension_name", "0.0.1")
    .func(make_func<&my_function_impl>("my_function")
      .returns(STRING)  // or INT, etc.
      .param(STRING)    // add .param() for each parameter
      .param(INT)
      .buffer_size(1024)  // max result size
      .build())
)
```

### Key Differences from Old MySQL UDF API:
- No separate init/main/deinit functions - single implementation function
- Arguments passed as `vef_invalue_t*` structs with `is_null`, `str_value`, `bin_value`, `int_value` fields
- Results set via `vef_vdf_result_t*` with `type`, `str_buf`, `bin_buf`, `actual_len` fields
- Function registration done declaratively in code using builder pattern
- No install.sql needed - functions registered at extension load time

## Testing

The extension includes test files using the MySQL Test Runner (MTR) framework:
- **Test Location**:
  - `test/t/` directory contains `.test` files with SQL test commands
  - `test/r/` directory contains `.result` files with expected output
- **Test Files**:
  - `vsql_http_encode.test` - Tests URL encode/decode functions
  - `vsql_http_requests.test` - Tests all HTTP methods against a local `python3 -m http.server`
  - `vsql_http_live.test` - Tests real HTTPS against `villagesql.com/robots.txt` (skip when offline with `--skip-test=vsql_http_live`)

### Running Tests

**Option 1 (Default): Using installed VEB**

This method assumes the VEB is already installed to your VillageSQL veb_dir:

```bash
cd /path/to/mysql-test
perl mysql-test-run.pl --suite=/path/to/vsql-http/test
```

**Option 2: Using a specific VEB file**

Use this to test a specific VEB build without installing it first:

```bash
cd /path/to/mysql-test
VSQL_HTTP_VEB=/path/to/vsql-http/build/vsql_http.veb \
  perl mysql-test-run.pl --suite=/path/to/vsql-http/test
```

### Creating or Updating Test Results

Use `--record` flag to generate or update expected `.result` files:

```bash
cd /path/to/mysql-test
VSQL_HTTP_VEB=/path/to/vsql-http/build/vsql_http.veb \
  perl mysql-test-run.pl --suite=/path/to/vsql-http/test --record
```

### Test Guidelines

- Tests should validate function output and behavior
- Each test should install the extension, run tests, and clean up (uninstall extension)
- HTTP tests use a local `python3 -m http.server` — no external network access required
- **Error Handling**: Functions return NULL for errors (result->type = VEF_RESULT_NULL)

## Extension Installation

After building the extension, install it in VillageSQL:

```sql
INSTALL EXTENSION vsql_http;
```

Then test the functions:
```sql
SELECT vsql_http.http_get('https://api.example.com/data');
SELECT vsql_http.url_encode('hello world & more');
SELECT vsql_http.url_decode('hello%20world%20%26%20more');
```

## Adding New HTTP Functions

To add new functions to this extension:

1. **Implement the VDF in `src/vsql_http.cc`**:
   - Add the implementation function with signature: `void func_impl(vef_context_t*, vef_invalue_t*..., vef_vdf_result_t*)`
   - Use the `http_call()` template wrapper for HTTP functions or `curl_codec_impl()` for encode/decode functions
   - Check for NULL arguments and set `result->type = VEF_RESULT_NULL` on error
   - Include copyright header if creating new files

2. **Register the function in the extension**:
   - Add function registration in `VEF_GENERATE_ENTRY_POINTS` block
   - Use `make_func<&func_impl>("function_name")` with appropriate `.returns()`, `.param()`, and `.buffer_size()` settings

3. **Create tests**:
   - Add tests to existing test files or create new `.test` files in `test/t/`
   - Generate expected results using `--record` flag
   - Test various inputs including edge cases, NULL values, and error conditions

4. **Update documentation**:
   - Add function descriptions to README.md
   - Update AGENTS.md with new function signatures

## Licensing and Copyright

All source code files (`.cc`, `.h`, `.cpp`, `.hpp`) and CMake files (`CMakeLists.txt`) must include the following copyright header at the top of the file:

```
// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
```

When creating new source files, always include this copyright block before any code or includes.

## Common Tasks for AI Agents

When asked to add functionality to this extension:

1. **Adding a new function**: Create the VDF implementation in src/vsql_http.cc, register it in VEF_GENERATE_ENTRY_POINTS, create tests
2. **Modifying build**: Edit CMakeLists.txt, ensure proper library linking
3. **Adding dependencies**: Update CMakeLists.txt with find_package() or target_link_libraries()
4. **Testing**:
   - Create or update `.test` files in `test/t/` directory
   - Generate expected results with `--record` flag
   - Verify all tests pass with `perl mysql-test-run.pl --suite=<path>`
5. **Documentation**: Update README.md and AGENTS.md to reflect new functionality

Always maintain consistency with existing code style and include proper copyright headers.
